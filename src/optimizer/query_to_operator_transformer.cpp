#include "optimizer/query_to_operator_transformer.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "catalog/catalog_accessor.h"
#include "common/macros.h"
#include "common/managed_pointer.h"
#include "loggers/optimizer_logger.h"
#include "optimizer/logical_operators.h"
#include "optimizer/operator_node.h"
#include "parser/expression/column_value_expression.h"
#include "parser/expression/comparison_expression.h"
#include "parser/expression/operator_expression.h"
#include "parser/expression/subquery_expression.h"
#include "parser/expression_util.h"
#include "parser/postgresparser.h"
#include "parser/statements.h"
#include "planner/plannodes/plan_node_defs.h"

namespace noisepage::optimizer {

QueryToOperatorTransformer::QueryToOperatorTransformer(
    const common::ManagedPointer<catalog::CatalogAccessor> catalog_accessor, const catalog::db_oid_t db_oid)
    : accessor_(catalog_accessor), db_oid_(db_oid) {
  output_expr_ = nullptr;
}

std::unique_ptr<AbstractOptimizerNode> QueryToOperatorTransformer::ConvertToOpExpression(
    common::ManagedPointer<parser::SQLStatement> op, common::ManagedPointer<parser::ParseResult> parse_result) {
  output_expr_ = nullptr;
  parse_result_ = parse_result;

  op->Accept(common::ManagedPointer(this).CastManagedPointerTo<SqlNodeVisitor>());
  return std::move(output_expr_);
}

bool QueryToOperatorTransformer::FindFirstCTEScanNode(common::ManagedPointer<AbstractOptimizerNode> child_expr,
                                                      const std::string &cte_table_name) {
  bool is_added = false;

  // TODO(preetang): Replace explicit string usage for operator name with reference to constant string
  if (child_expr->Contents()->GetOpType() == OpType::LOGICALCTESCAN &&
      (child_expr->Contents()->GetContentsAs<LogicalCteScan>()->GetTableName() == cte_table_name)) {
    // Leftmost LogicalCteScan found in tree
    child_expr->PushChild(std::move(output_expr_));
    is_added = true;
  } else {
    auto children = child_expr->GetChildren();
    for (auto &i : children) {
      is_added = FindFirstCTEScanNode(i, cte_table_name);
      if (is_added) break;
    }
  }

  return is_added;
}

void QueryToOperatorTransformer::Visit(common::ManagedPointer<parser::SelectStatement> op) {
  OPTIMIZER_LOG_DEBUG("Transforming SelectStatement to operators ...");
  // We do not visit the select list of a base table because the column
  // information is derived before the plan generation, at this step we
  // don't need to derive that
  auto pre_predicates = std::move(predicates_);
  predicates_ = {};
  transaction::TransactionContext *txn_context = accessor_->GetTxn().Get();

  if (!op->GetSelectWith().empty()) {
    for (auto with : op->GetSelectWith()) {
      with->Accept(common::ManagedPointer(this).CastManagedPointerTo<SqlNodeVisitor>());
      // SELECT statement has CTE, register CTE table name
      cte_table_name_.push_back(with->GetAlias().GetName());
      cte_type_.push_back(with->GetCteType());

      auto oid = catalog::MakeTempOid<catalog::table_oid_t>(accessor_->GetNewTempOid());
      cte_oids_.push_back(oid);

      std::vector<execution::sql::SqlTypeId> col_types{};
      col_types.reserve(with->GetCteColumnAliases().size());
      for (auto i = 0UL; i < with->GetCteColumnAliases().size(); ++i) {
        col_types.push_back(with->GetSelect()->GetSelectColumns()[i]->GetReturnValueType());
      }
      std::vector<catalog::Schema::Column> columns1;
      std::size_t i = 0;
      for (auto &alias : with->GetCteColumnAliases()) {
        columns1.emplace_back(alias.GetName(), col_types[i], false, parser::ConstantValueExpression(col_types[i]),
                              catalog::MakeTempOid<catalog::col_oid_t>(alias.GetSerialNo().UnderlyingValue()));
        i++;
      }

      cte_schemas_.emplace_back(catalog::Schema(std::move(columns1)));
      std::vector<std::vector<common::ManagedPointer<parser::AbstractExpression>>> master_expressions;
      std::vector<common::ManagedPointer<parser::AbstractExpression>> expressions;

      std::size_t index = 0;
      for (auto &elem : with->GetCteColumnAliases()) {
        NOISEPAGE_ASSERT(elem.IsSerialNoValid(), "CTE Alias does not have a valid serial no.");
        auto ret_type = with->GetSelect()->GetSelectColumns()[index]->GetReturnValueType();
        parser::AbstractExpression *cve = new parser::ColumnValueExpression(
            with->GetAlias(), elem.GetName(), ret_type, elem,
            catalog::MakeTempOid<catalog::col_oid_t>(elem.GetSerialNo().UnderlyingValue()));
        txn_context->RegisterAbortAction([=] { delete cve; });
        txn_context->RegisterCommitAction([=] { delete cve; });
        expressions.emplace_back(common::ManagedPointer(cve));

        col_types.push_back(ret_type);
        cve->SetReturnValueType(ret_type);
        index++;
      }

      master_expressions.push_back(std::move(expressions));
      std::vector<catalog::Schema::Column> columns;
      size_t ind = 0;
      for (auto &alias : with->GetCteColumnAliases()) {
        columns.emplace_back(alias.GetName(), col_types[ind], false, parser::ConstantValueExpression(col_types[ind]),
                             catalog::MakeTempOid<catalog::col_oid_t>(alias.GetSerialNo().UnderlyingValue()));
        ind++;
      }

      auto cte_scan_expr = std::make_unique<OperatorNode>(
          LogicalCteScan::Make(with->GetAlias().GetName(), with->GetTableName(), oid,
                               catalog::Schema(std::move(columns)), {}, with->GetCteType(), {})
              .RegisterWithTxnContext(txn_context),
          std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      cte_scan_expr->PushChild(std::move(output_expr_));
      output_expr_ = std::move(cte_scan_expr);

      if (with->GetSelect()->GetUnionSelect() != nullptr) {
        std::vector<common::ManagedPointer<parser::AbstractExpression>> second_expressions;
        auto &second_columns = with->GetSelect()->GetUnionSelect()->GetSelectColumns();
        second_expressions.reserve(second_columns.size());
        for (auto &elems : second_columns) {
          second_expressions.push_back(elems);
        }
        master_expressions.push_back(std::move(second_expressions));
      }
      cte_expressions_.push_back(master_expressions);
    }
  }

  if (op->GetSelectTable() != nullptr) {
    // SELECT with FROM
    op->GetSelectTable()->Accept(common::ManagedPointer(this).CastManagedPointerTo<SqlNodeVisitor>());
  } else {
    // SELECT without FROM
    output_expr_ = std::make_unique<OperatorNode>(LogicalGet::Make().RegisterWithTxnContext(txn_context),
                                                  std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
  }

  if (op->GetSelectCondition() != nullptr) {
    OPTIMIZER_LOG_DEBUG("Collecting predicates ...");
    CollectPredicates(op->GetSelectCondition(), &predicates_);
  }

  if (!predicates_.empty()) {
    auto filter_expr =
        std::make_unique<OperatorNode>(LogicalFilter::Make(std::move(predicates_)).RegisterWithTxnContext(txn_context),
                                       std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
    filter_expr->PushChild(std::move(output_expr_));
    predicates_.clear();
    output_expr_ = std::move(filter_expr);
  }

  if (QueryToOperatorTransformer::RequireAggregation(common::ManagedPointer(op))) {
    OPTIMIZER_LOG_DEBUG("Handling aggregation in SelectStatement ...");
    // Plain aggregation
    std::unique_ptr<OperatorNode> agg_expr;
    if (op->GetSelectGroupBy() == nullptr) {
      // TODO(boweic): aggregation without groupby could still have having clause
      agg_expr = std::make_unique<OperatorNode>(LogicalAggregateAndGroupBy::Make().RegisterWithTxnContext(txn_context),
                                                std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      agg_expr->PushChild(std::move(output_expr_));
      output_expr_ = std::move(agg_expr);
    } else {
      size_t num_group_by_cols = op->GetSelectGroupBy()->GetColumns().size();
      auto group_by_cols = std::vector<common::ManagedPointer<parser::AbstractExpression>>(num_group_by_cols);
      for (size_t i = 0; i < num_group_by_cols; i++) {
        group_by_cols[i] = common::ManagedPointer<parser::AbstractExpression>(op->GetSelectGroupBy()->GetColumns()[i]);
      }
      agg_expr = std::make_unique<OperatorNode>(
          LogicalAggregateAndGroupBy::Make(std::move(group_by_cols)).RegisterWithTxnContext(txn_context),
          std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      agg_expr->PushChild(std::move(output_expr_));
      output_expr_ = std::move(agg_expr);

      std::vector<AnnotatedExpression> having;
      if (op->GetSelectGroupBy()->GetHaving() != nullptr) {
        CollectPredicates(op->GetSelectGroupBy()->GetHaving(), &having);
      }
      if (!having.empty()) {
        auto filter_expr =
            std::make_unique<OperatorNode>(LogicalFilter::Make(std::move(having)).RegisterWithTxnContext(txn_context),
                                           std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
        filter_expr->PushChild(std::move(output_expr_));
        output_expr_ = std::move(filter_expr);
      }
    }
  } else if (op->IsSelectDistinct()) {
    // SELECT DISTINCT a1 FROM A should be transformed to
    // SELECT a1 FROM A GROUP BY a1
    auto num_cols = op->GetSelectColumns().size();
    auto group_by_cols = std::vector<common::ManagedPointer<parser::AbstractExpression>>(num_cols);
    for (size_t i = 0; i < num_cols; i++) {
      group_by_cols[i] = common::ManagedPointer<parser::AbstractExpression>(op->GetSelectColumns()[i]);
    }

    std::vector<std::unique_ptr<AbstractOptimizerNode>> c;
    c.emplace_back(std::move(output_expr_));
    output_expr_ = std::make_unique<OperatorNode>(
        LogicalAggregateAndGroupBy::Make(std::move(group_by_cols)).RegisterWithTxnContext(txn_context), std::move(c),
        accessor_->GetTxn().Get());
  }

  if (op->GetSelectLimit() != nullptr && op->GetSelectLimit()->GetLimit() != -1) {
    OPTIMIZER_LOG_DEBUG("Handling order by/limit/offset in SelectStatement ...");
    std::vector<common::ManagedPointer<parser::AbstractExpression>> sort_exprs;
    std::vector<optimizer::OrderByOrderingType> sort_direction;

    if (op->GetSelectOrderBy() != nullptr) {
      const auto &order_info = op->GetSelectOrderBy();
      for (auto &expr : order_info->GetOrderByExpressions()) {
        sort_exprs.push_back(expr);
      }
      for (auto &type : order_info->GetOrderByTypes()) {
        if (type == parser::kOrderAsc)
          sort_direction.push_back(optimizer::OrderByOrderingType::ASC);
        else
          sort_direction.push_back(optimizer::OrderByOrderingType::DESC);
      }
    }
    auto limit_expr = std::make_unique<OperatorNode>(
        LogicalLimit::Make(std::max(op->GetSelectLimit()->GetOffset(), static_cast<int64_t>(0)),
                           op->GetSelectLimit()->GetLimit(), std::move(sort_exprs), std::move(sort_direction))
            .RegisterWithTxnContext(txn_context),
        std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
    limit_expr->PushChild(std::move(output_expr_));
    output_expr_ = std::move(limit_expr);
  }

  if (!op->GetSelectWith().empty()) {
    // Store the current logical tree in another expression
    auto child_expr = std::move(output_expr_);

    for (auto with : op->GetSelectWith()) {
      // Get the logical tree for the query which is used to compute the CTE table
      with->Accept(common::ManagedPointer(this).CastManagedPointerTo<SqlNodeVisitor>());
      // Add CTE table query to first LogicalCteScan found in tree
      // TODO(tanujnay112) think about this more, there might be a better way
      FindFirstCTEScanNode(common::ManagedPointer(child_expr).CastManagedPointerTo<AbstractOptimizerNode>(),
                           with->GetAlias().GetName());
    }

    // Replace the complete logical tree back
    output_expr_ = std::move(child_expr);
  }

  if (op->GetUnionSelect() != nullptr) {
    auto left_expr = std::move(output_expr_);
    op->GetUnionSelect()->Accept(common::ManagedPointer(this).CastManagedPointerTo<SqlNodeVisitor>());
    auto right_expr = std::move(output_expr_);
    // TODO(Kyle): below we just hard-code the is_all flag, we need to fix this
    // by getting the parser to represent whether it is UNION or UNION ALL
    output_expr_ = std::make_unique<OperatorNode>(
        LogicalUnion::Make(true, op, op->GetUnionSelect()).RegisterWithTxnContext(txn_context),
        std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
    output_expr_->PushChild(std::move(left_expr));
    output_expr_->PushChild(std::move(right_expr));
  }

  predicates_ = std::move(pre_predicates);
}

void QueryToOperatorTransformer::Visit(common::ManagedPointer<parser::JoinDefinition> node) {
  OPTIMIZER_LOG_DEBUG("Transforming JoinDefinition to operators ...");
  // Get left operator
  node->GetLeftTable()->Accept(common::ManagedPointer(this).CastManagedPointerTo<SqlNodeVisitor>());
  auto left_expr = std::move(output_expr_);

  // Get right operator
  node->GetRightTable()->Accept(common::ManagedPointer(this).CastManagedPointerTo<SqlNodeVisitor>());
  auto right_expr = std::move(output_expr_);

  // Construct join operator
  std::unique_ptr<OperatorNode> join_expr;
  std::vector<AnnotatedExpression> join_predicates;
  CollectPredicates(node->GetJoinCondition(), &join_predicates);
  transaction::TransactionContext *txn_context = accessor_->GetTxn().Get();
  switch (node->GetJoinType()) {
    case parser::JoinType::INNER: {
      join_expr = std::make_unique<OperatorNode>(
          LogicalInnerJoin::Make(std::move(join_predicates)).RegisterWithTxnContext(txn_context),
          std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      join_expr->PushChild(std::move(left_expr));
      join_expr->PushChild(std::move(right_expr));
      break;
    }
    case parser::JoinType::OUTER: {
      join_expr = std::make_unique<OperatorNode>(
          LogicalOuterJoin::Make(std::move(join_predicates)).RegisterWithTxnContext(txn_context),
          std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      join_expr->PushChild(std::move(left_expr));
      join_expr->PushChild(std::move(right_expr));
      break;
    }
    case parser::JoinType::LEFT: {
      join_expr = std::make_unique<OperatorNode>(
          LogicalLeftJoin::Make(std::move(join_predicates)).RegisterWithTxnContext(txn_context),
          std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      join_expr->PushChild(std::move(left_expr));
      join_expr->PushChild(std::move(right_expr));
      break;
    }
    case parser::JoinType::RIGHT: {
      join_expr = std::make_unique<OperatorNode>(
          LogicalRightJoin::Make(std::move(join_predicates)).RegisterWithTxnContext(txn_context),
          std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      join_expr->PushChild(std::move(left_expr));
      join_expr->PushChild(std::move(right_expr));
      break;
    }
    case parser::JoinType::SEMI: {
      join_expr = std::make_unique<OperatorNode>(
          LogicalSemiJoin::Make(std::move(join_predicates)).RegisterWithTxnContext(txn_context),
          std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      join_expr->PushChild(std::move(left_expr));
      join_expr->PushChild(std::move(right_expr));
      break;
    }
    default:
      throw OPTIMIZER_EXCEPTION("Join type invalid");
  }

  output_expr_ = std::move(join_expr);
}

void QueryToOperatorTransformer::Visit(common::ManagedPointer<parser::TableRef> node) {
  OPTIMIZER_LOG_DEBUG("Transforming TableRef to operators ...");

  transaction::TransactionContext *txn_context = accessor_->GetTxn().Get();
  if (node->GetSelect() != nullptr) {
    // Store previous context

    // Construct query derived table predicates
    // i.e. the mapping from column name to the underlying expression in the sub-query.
    // This is needed to generate input/output information for subqueries
    auto &table_alias = node->GetAlias();
    auto table_alias_name = table_alias.GetName();
    std::transform(table_alias_name.begin(), table_alias_name.end(), table_alias_name.begin(), ::tolower);
    table_alias.SetName(table_alias_name);

    auto alias_to_expr_map = ConstructSelectElementMap(node->GetSelect()->GetSelectColumns());

    node->GetSelect()->Accept(common::ManagedPointer(this).CastManagedPointerTo<SqlNodeVisitor>());

    auto child_expr = std::move(output_expr_);
    output_expr_ = std::make_unique<OperatorNode>(
        LogicalQueryDerivedGet::Make(table_alias, std::move(alias_to_expr_map)).RegisterWithTxnContext(txn_context),
        std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
    output_expr_->PushChild(std::move(child_expr));
  } else if (node->GetJoin() != nullptr) {
    // Explicit Join
    node->GetJoin()->Accept(common::ManagedPointer(this).CastManagedPointerTo<SqlNodeVisitor>());
  } else if (node->GetList().size() > 1) {
    // Multiple tables (Implicit Join)
    // Create a join operator between the first two tables
    node->GetList().at(0)->Accept(common::ManagedPointer(this).CastManagedPointerTo<SqlNodeVisitor>());
    auto prev_expr = std::move(output_expr_);
    // Build a left deep join tree
    for (size_t i = 1; i < node->GetList().size(); i++) {
      // Start at i = 1 due to the Accept() above
      auto list_elem = node->GetList().at(i);

      list_elem->Accept(common::ManagedPointer(this).CastManagedPointerTo<SqlNodeVisitor>());
      auto join_expr =
          std::make_unique<OperatorNode>(LogicalInnerJoin::Make().RegisterWithTxnContext(txn_context),
                                         std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      join_expr->PushChild(std::move(prev_expr));
      join_expr->PushChild(std::move(output_expr_));
      NOISEPAGE_ASSERT(join_expr->GetChildren().size() == 2, "The join expr should have exactly 2 elements");
      prev_expr = std::move(join_expr);
    }
    output_expr_ = std::move(prev_expr);
  } else {
    // Single table
    if (node->GetList().size() == 1) node = node->GetList().at(0).Get();

    auto it = std::find(cte_table_name_.begin(), cte_table_name_.end(), node->GetTableName());
    if (it != cte_table_name_.end()) {
      // CTE table referred
      const auto index = std::distance(cte_table_name_.begin(), it);
      std::vector<execution::sql::SqlTypeId> col_types{};
      auto cte_scan_expr = std::make_unique<OperatorNode>(
          LogicalCteScan::Make(node->GetAlias().GetName(), node->GetTableName(), cte_oids_[index], cte_schemas_[index],
                               cte_expressions_[index], cte_type_[index], {})
              .RegisterWithTxnContext(txn_context),
          std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      output_expr_ = std::move(cte_scan_expr);
    } else {
      // TODO(Ling): how should we determine the value of `is_for_update` field of logicalGet constructor?
      output_expr_ = std::make_unique<OperatorNode>(
          LogicalGet::Make(db_oid_, accessor_->GetTableOid(node->GetTableName()), {}, node->GetAlias(), false)
              .RegisterWithTxnContext(txn_context),
          std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
    }
  }
}

void QueryToOperatorTransformer::Visit(common::ManagedPointer<parser::GroupByDescription> node) {
  OPTIMIZER_LOG_DEBUG("Transforming GroupByDescription to operators ...");
}
void QueryToOperatorTransformer::Visit(common::ManagedPointer<parser::OrderByDescription> node) {
  OPTIMIZER_LOG_DEBUG("Transforming OrderByDescription to operators ...");
}
void QueryToOperatorTransformer::Visit(UNUSED_ATTRIBUTE common::ManagedPointer<parser::LimitDescription> node) {
  OPTIMIZER_LOG_DEBUG("Transforming LimitDescription to operators ...");
}
void QueryToOperatorTransformer::Visit(common::ManagedPointer<parser::CreateFunctionStatement> op) {
  OPTIMIZER_LOG_DEBUG("Transforming CreateFunctionStatement to operators ...");
  // TODO(Ling): Where should the as_type_ go?
  std::vector<std::string> function_param_names;
  std::vector<parser::BaseFunctionParameter::DataType> function_param_types;
  transaction::TransactionContext *txn_context = accessor_->GetTxn().Get();
  for (const auto &col : op->GetFuncParameters()) {
    function_param_names.push_back(col->GetParamName());
    function_param_types.push_back(col->GetDataType());
  }
  // TODO(Ling): database oid of create function?
  auto create_expr = std::make_unique<OperatorNode>(
      LogicalCreateFunction::Make(catalog::INVALID_DATABASE_OID, accessor_->GetDefaultNamespace(), op->GetFuncName(),
                                  op->GetPLType(), op->GetFuncBody(), std::move(function_param_names),
                                  std::move(function_param_types), op->GetFuncReturnType()->GetDataType(),
                                  op->GetFuncParameters().size(), op->ShouldReplace())
          .RegisterWithTxnContext(txn_context),
      std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
  output_expr_ = std::move(create_expr);
}

void QueryToOperatorTransformer::Visit(common::ManagedPointer<parser::CreateStatement> op) {
  OPTIMIZER_LOG_DEBUG("Transforming CreateStatement to operators ...");
  auto create_type = op->GetCreateType();
  std::unique_ptr<OperatorNode> create_expr;
  transaction::TransactionContext *txn_context = accessor_->GetTxn().Get();
  switch (create_type) {
    case parser::CreateStatement::CreateType::kDatabase:
      create_expr = std::make_unique<OperatorNode>(
          LogicalCreateDatabase::Make(op->GetDatabaseName()).RegisterWithTxnContext(txn_context),
          std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      break;
    case parser::CreateStatement::CreateType::kTable:
      create_expr = std::make_unique<OperatorNode>(
          LogicalCreateTable::Make(accessor_->GetNamespaceOid(op->GetNamespaceName()), op->GetTableName(),
                                   op->GetColumns(), op->GetForeignKeys())
              .RegisterWithTxnContext(txn_context),
          std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      // TODO(Ling): for other procedures to generate create table plan, refer to create_table_plan_node builder.
      //  Following part might be more adequate to be handled by optimizer when it it actually constructing the plan
      //  I don't think we should extract out the desired fields here.
      break;
    case parser::CreateStatement::CreateType::kIndex: {
      // create vector of expressions of the index entires
      std::vector<common::ManagedPointer<parser::AbstractExpression>> entries;
      for (auto &attr : op->GetIndexAttributes()) {
        if (attr.HasExpr()) {
          entries.emplace_back(attr.GetExpression());
        } else {
          // TODO(Matt): can an index attribute definition ever reference multiple tables? I don't think so. We should
          // probably move this out of the loop.
          // TODO(Matt): why are we still binding names to oids at this point? Didn't the binder already bind this
          // expression?
          const auto tb_oid = accessor_->GetTableOid(op->GetTableName());
          const auto &table_schema = accessor_->GetSchema(tb_oid);
          const auto &table_col = table_schema.GetColumn(attr.GetName());
          auto unique_col_expr =
              std::make_unique<parser::ColumnValueExpression>(parser::AliasType(op->GetTableName()), attr.GetName(),
                                                              db_oid_, tb_oid, table_col.Oid(), table_col.Type());
          parse_result_->AddExpression(std::move(unique_col_expr));
          auto new_col_expr = common::ManagedPointer(parse_result_->GetExpressions().back());
          entries.push_back(new_col_expr);
        }
      }
      create_expr = std::make_unique<OperatorNode>(
          LogicalCreateIndex::Make(db_oid_, accessor_->GetDefaultNamespace(),
                                   accessor_->GetTableOid(op->GetTableName()), op->GetIndexType(), op->IsUniqueIndex(),
                                   op->GetIndexName(), std::move(entries), op->MoveIndexOptions())
              .RegisterWithTxnContext(txn_context),
          std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      break;
    }
    case parser::CreateStatement::CreateType::kTrigger: {
      std::vector<catalog::col_oid_t> trigger_columns;
      auto tb_oid = accessor_->GetTableOid(op->GetTableName());
      auto schema = accessor_->GetSchema(tb_oid);
      for (const auto &col : op->GetTriggerColumns()) trigger_columns.emplace_back(schema.GetColumn(col).Oid());
      create_expr = std::make_unique<OperatorNode>(

          LogicalCreateTrigger::Make(db_oid_, accessor_->GetDefaultNamespace(), tb_oid, op->GetTriggerName(),
                                     op->GetTriggerFuncNames(), op->GetTriggerArgs(), std::move(trigger_columns),
                                     op->GetTriggerWhen(), op->GetTriggerType())
              .RegisterWithTxnContext(txn_context),
          std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      break;
    }
    case parser::CreateStatement::CreateType::kSchema:
      create_expr = std::make_unique<OperatorNode>(
          LogicalCreateNamespace::Make(op->GetNamespaceName()).RegisterWithTxnContext(txn_context),
          std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      break;
    case parser::CreateStatement::CreateType::kView:
      create_expr = std::make_unique<OperatorNode>(

          LogicalCreateView::Make(db_oid_, accessor_->GetDefaultNamespace(), op->GetViewName(), op->GetViewQuery())
              .RegisterWithTxnContext(txn_context),
          std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      break;
  }

  output_expr_ = std::move(create_expr);
}
void QueryToOperatorTransformer::Visit(common::ManagedPointer<parser::InsertStatement> op) {
  OPTIMIZER_LOG_DEBUG("Transforming InsertStatement to operators ...");
  auto target_table = op->GetInsertionTable();
  auto target_table_id = accessor_->GetTableOid(target_table->GetTableName());
  auto target_db_id = db_oid_;
  transaction::TransactionContext *txn_context = accessor_->GetTxn().Get();
  bool is_select_insert = op->GetInsertType() == parser::InsertType::SELECT;

  if (is_select_insert) {
    ValidateInsertValues(op, op->GetSelect()->GetSelectColumns(), target_table_id);
  } else {
    for (const auto &values : *(op->GetValues())) {
      ValidateInsertValues(op, values, target_table_id);
    }
  }

  // vector of column oids
  std::vector<catalog::col_oid_t> col_ids;

  // The set below contains names of columns mentioned in the insert statement
  std::unordered_set<catalog::col_oid_t> specified;
  auto schema = accessor_->GetSchema(target_table_id);

  for (const auto &col : *(op->GetInsertColumns())) {
    try {
      const auto &column_object = schema.GetColumn(col);
      specified.insert(column_object.Oid());
      col_ids.emplace_back(column_object.Oid());
    } catch (const std::out_of_range &oor) {
      throw CATALOG_EXCEPTION(
          ("Column \"" + col + "\" of relation \"" + target_table->GetTableName() + "\" does not exist").c_str());
    }
  }

  for (const auto &column : schema.GetColumns()) {
    // this loop checks not null constraint for unspecified columns
    if (specified.find(column.Oid()) == specified.end() && !column.Nullable() && column.StoredExpression() == nullptr) {
      throw CATALOG_EXCEPTION(("Null value in column \"" + column.Name() + "\" violates not-null constraint").c_str());
    }
  }

  if (is_select_insert) {
    auto insert_expr =
        std::make_unique<OperatorNode>(LogicalInsertSelect::Make(target_db_id, target_table_id, std::move(col_ids))
                                           .RegisterWithTxnContext(txn_context),
                                       std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
    op->GetSelect()->Accept(common::ManagedPointer(this).CastManagedPointerTo<SqlNodeVisitor>());

    insert_expr->PushChild(std::move(output_expr_));
    output_expr_ = std::move(insert_expr);
  } else {
    auto insert_expr = std::make_unique<OperatorNode>(
        LogicalInsert::Make(target_db_id, target_table_id, std::move(col_ids), op->GetValues())
            .RegisterWithTxnContext(txn_context),
        std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
    output_expr_ = std::move(insert_expr);
  }
}

void QueryToOperatorTransformer::Visit(common::ManagedPointer<parser::DeleteStatement> op) {
  OPTIMIZER_LOG_DEBUG("Transforming DeleteStatement to operators ...");
  auto target_table = op->GetDeletionTable();
  auto target_db_id = db_oid_;
  auto target_table_id = accessor_->GetTableOid(target_table->GetTableName());
  const auto &target_table_alias = target_table->GetAlias();
  transaction::TransactionContext *txn_context = accessor_->GetTxn().Get();

  std::vector<std::unique_ptr<AbstractOptimizerNode>> c;
  auto delete_expr =
      std::make_unique<OperatorNode>(LogicalDelete::Make(target_db_id, target_table_alias.GetName(), target_table_id)
                                         .RegisterWithTxnContext(txn_context),
                                     std::move(c), txn_context);

  std::unique_ptr<OperatorNode> table_scan;
  if (op->GetDeleteCondition() != nullptr) {
    std::vector<AnnotatedExpression> predicates;
    QueryToOperatorTransformer::ExtractPredicates(op->GetDeleteCondition(), &predicates);
    table_scan = std::make_unique<OperatorNode>(
        LogicalGet::Make(target_db_id, target_table_id, predicates, target_table_alias, true)
            .RegisterWithTxnContext(txn_context),
        std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
  } else {
    table_scan =
        std::make_unique<OperatorNode>(LogicalGet::Make(target_db_id, target_table_id, {}, target_table_alias, true)
                                           .RegisterWithTxnContext(txn_context),
                                       std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
  }
  delete_expr->PushChild(std::move(table_scan));

  output_expr_ = std::move(delete_expr);
}

void QueryToOperatorTransformer::Visit(common::ManagedPointer<parser::DropStatement> op) {
  OPTIMIZER_LOG_DEBUG("Transforming DropStatement to operators ...");
  auto drop_type = op->GetDropType();
  transaction::TransactionContext *txn_context = accessor_->GetTxn().Get();
  std::unique_ptr<OperatorNode> drop_expr;
  switch (drop_type) {
    case parser::DropStatement::DropType::kDatabase:
      drop_expr = std::make_unique<OperatorNode>(LogicalDropDatabase::Make(db_oid_).RegisterWithTxnContext(txn_context),
                                                 std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      break;
    case parser::DropStatement::DropType::kTable:
      drop_expr = std::make_unique<OperatorNode>(
          LogicalDropTable::Make(accessor_->GetTableOid(op->GetTableName())).RegisterWithTxnContext(txn_context),
          std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      break;
    case parser::DropStatement::DropType::kIndex:
      drop_expr = std::make_unique<OperatorNode>(
          LogicalDropIndex::Make(accessor_->GetIndexOid(op->GetIndexName())).RegisterWithTxnContext(txn_context),
          std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      break;
    case parser::DropStatement::DropType::kSchema:
      drop_expr =
          std::make_unique<OperatorNode>(LogicalDropNamespace::Make(accessor_->GetNamespaceOid(op->GetNamespaceName()))
                                             .RegisterWithTxnContext(txn_context),
                                         std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
      break;
    case parser::DropStatement::DropType::kTrigger:
    case parser::DropStatement::DropType::kView:
    case parser::DropStatement::DropType::kPreparedStatement:
      break;
  }

  output_expr_ = std::move(drop_expr);
}
void QueryToOperatorTransformer::Visit(UNUSED_ATTRIBUTE common::ManagedPointer<parser::PrepareStatement> op) {
  OPTIMIZER_LOG_DEBUG("Transforming PrepareStatement to operators ...");
}
void QueryToOperatorTransformer::Visit(UNUSED_ATTRIBUTE common::ManagedPointer<parser::ExecuteStatement> op) {
  OPTIMIZER_LOG_DEBUG("Transforming ExecuteStatement to operators ...");
}
void QueryToOperatorTransformer::Visit(UNUSED_ATTRIBUTE common::ManagedPointer<parser::ExplainStatement> op) {
  OPTIMIZER_LOG_DEBUG("Transforming ExplainStatement to operators ...");
}
void QueryToOperatorTransformer::Visit(UNUSED_ATTRIBUTE common::ManagedPointer<parser::TransactionStatement> op) {
  OPTIMIZER_LOG_DEBUG("Transforming Transaction to operators ...");
}

void QueryToOperatorTransformer::Visit(common::ManagedPointer<parser::UpdateStatement> op) {
  OPTIMIZER_LOG_DEBUG("Transforming UpdateStatement to operators ...");
  auto target_table = op->GetUpdateTable();
  auto target_db_id = db_oid_;
  auto target_table_id = accessor_->GetTableOid(target_table->GetTableName());
  const auto &target_table_alias = target_table->GetAlias();
  transaction::TransactionContext *txn_context = accessor_->GetTxn().Get();

  std::unique_ptr<OperatorNode> table_scan;

  std::vector<std::unique_ptr<AbstractOptimizerNode>> c;
  auto update_expr = std::make_unique<OperatorNode>(
      LogicalUpdate::Make(target_db_id, target_table_alias.GetName(), target_table_id, op->GetUpdateClauses())
          .RegisterWithTxnContext(txn_context),
      std::move(c), txn_context);

  if (op->GetUpdateCondition() != nullptr) {
    std::vector<AnnotatedExpression> predicates;
    QueryToOperatorTransformer::ExtractPredicates(op->GetUpdateCondition(), &predicates);
    table_scan = std::make_unique<OperatorNode>(
        LogicalGet::Make(target_db_id, target_table_id, predicates, target_table_alias, true)
            .RegisterWithTxnContext(txn_context),
        std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
  } else {
    table_scan =
        std::make_unique<OperatorNode>(LogicalGet::Make(target_db_id, target_table_id, {}, target_table_alias, true)
                                           .RegisterWithTxnContext(txn_context),
                                       std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
  }
  update_expr->PushChild(std::move(table_scan));

  output_expr_ = std::move(update_expr);
}

void QueryToOperatorTransformer::Visit(common::ManagedPointer<parser::VariableSetStatement> op) {
  OPTIMIZER_LOG_DEBUG("Transforming VariableSetStatement to operators ...");
}

void QueryToOperatorTransformer::Visit(common::ManagedPointer<parser::CopyStatement> op) {
  OPTIMIZER_LOG_DEBUG("Transforming CopyStatement to operators ...");
  transaction::TransactionContext *txn_context = accessor_->GetTxn().Get();
  if (op->IsFrom()) {
    // The copy statement is reading from a file into a table. We construct a
    // logical external-file get operator as the leaf, and an insert operator
    // as the root.
    auto get_op = std::make_unique<OperatorNode>(
        LogicalExternalFileGet::Make(op->GetExternalFileFormat(), op->GetFilePath(), op->GetDelimiter(),
                                     op->GetQuoteChar(), op->GetEscapeChar())
            .RegisterWithTxnContext(txn_context),
        std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);

    auto target_table = op->GetCopyTable();
    auto table_oid = accessor_->GetTableOid(target_table->GetTableName());
    std::vector<catalog::col_oid_t> col_ids;
    for (auto &col : accessor_->GetSchema(table_oid).GetColumns()) {
      col_ids.emplace_back(col.Oid());
    }

    auto insert_op = std::make_unique<OperatorNode>(
        LogicalInsertSelect::Make(db_oid_, table_oid, std::move(col_ids)).RegisterWithTxnContext(txn_context),
        std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
    insert_op->PushChild(std::move(get_op));
    output_expr_ = std::move(insert_op);

  } else {
    if (op->GetSelectStatement() != nullptr) {
      op->GetSelectStatement()->Accept(common::ManagedPointer(this).CastManagedPointerTo<SqlNodeVisitor>());
    } else {
      op->GetCopyTable()->Accept(common::ManagedPointer(this).CastManagedPointerTo<SqlNodeVisitor>());
    }
    auto export_op = std::make_unique<OperatorNode>(
        LogicalExportExternalFile::Make(op->GetExternalFileFormat(), op->GetFilePath(), op->GetDelimiter(),
                                        op->GetQuoteChar(), op->GetEscapeChar())
            .RegisterWithTxnContext(txn_context),
        std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
    export_op->PushChild(std::move(output_expr_));

    output_expr_ = std::move(export_op);
  }
}

void QueryToOperatorTransformer::Visit(common::ManagedPointer<parser::AnalyzeStatement> op) {
  OPTIMIZER_LOG_DEBUG("Transforming AnalyzeStatement to operators ...");
  std::vector<catalog::col_oid_t> columns;
  auto db_oid = op->GetDatabaseOid();
  auto tb_oid = op->GetTableOid();

  for (auto col_oid : op->GetColumnOids()) {
    columns.emplace_back(col_oid);
  }

  auto analyze_expr = std::make_unique<OperatorNode>(
      LogicalAnalyze::Make(db_oid, tb_oid, std::move(columns)).RegisterWithTxnContext(accessor_->GetTxn().Get()),
      std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, accessor_->GetTxn().Get());
  auto aggregate_expr = std::make_unique<OperatorNode>(
      LogicalAggregateAndGroupBy::Make().RegisterWithTxnContext(accessor_->GetTxn().Get()),
      std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, accessor_->GetTxn().Get());
  auto get_expr =
      std::make_unique<OperatorNode>(LogicalGet::Make(db_oid, tb_oid, {}, op->GetAnalyzeTable()->GetAlias(), false)
                                         .RegisterWithTxnContext(accessor_->GetTxn().Get()),
                                     std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, accessor_->GetTxn().Get());
  aggregate_expr->PushChild(std::move(get_expr));
  analyze_expr->PushChild(std::move(aggregate_expr));
  output_expr_ = std::move(analyze_expr);
}

void QueryToOperatorTransformer::Visit(common::ManagedPointer<parser::ComparisonExpression> expr) {
  OPTIMIZER_LOG_DEBUG("Transforming ComparisonExpression to operators ...");
  auto expr_type = expr->GetExpressionType();
  if (expr->GetExpressionType() == parser::ExpressionType::COMPARE_IN) {
    GenerateSubqueryTree(expr.CastManagedPointerTo<parser::AbstractExpression>(), 1, false);
  } else if (expr_type == parser::ExpressionType::COMPARE_EQUAL ||
             expr_type == parser::ExpressionType::COMPARE_NOT_EQUAL ||
             expr_type == parser::ExpressionType::COMPARE_GREATER_THAN ||
             expr_type == parser::ExpressionType::COMPARE_GREATER_THAN_OR_EQUAL_TO ||
             expr_type == parser::ExpressionType::COMPARE_LESS_THAN ||
             expr_type == parser::ExpressionType::COMPARE_LESS_THAN_OR_EQUAL_TO) {
    if (expr->GetChild(0)->GetExpressionType() == parser::ExpressionType::ROW_SUBQUERY &&
        expr->GetChild(1)->GetExpressionType() == parser::ExpressionType::ROW_SUBQUERY) {
      throw NOT_IMPLEMENTED_EXCEPTION("Comparisons between sub-selects are not supported");
    }
    // Transform if either child is sub-query
    GenerateSubqueryTree(expr.CastManagedPointerTo<parser::AbstractExpression>(), 0, true) ||
        GenerateSubqueryTree(expr.CastManagedPointerTo<parser::AbstractExpression>(), 1, true);
  }
  expr->AcceptChildren(common::ManagedPointer(this).CastManagedPointerTo<SqlNodeVisitor>());
}

void QueryToOperatorTransformer::Visit(common::ManagedPointer<parser::OperatorExpression> expr) {
  OPTIMIZER_LOG_DEBUG("Transforming OperatorNode to operators ...");
  // TODO(boweic): We may want to do the rewrite (exist -> in) in the binder
  if (expr->GetExpressionType() == parser::ExpressionType::OPERATOR_EXISTS) {
    if (GenerateSubqueryTree(expr.CastManagedPointerTo<parser::AbstractExpression>(), 0, false)) {
      // Already reset the child to column, we need to transform exist to not-null to preserve semantic
      expr->SetExpressionType(parser::ExpressionType::OPERATOR_IS_NOT_NULL);
    }
  }

  expr->AcceptChildren(common::ManagedPointer(this).CastManagedPointerTo<SqlNodeVisitor>());
}

bool QueryToOperatorTransformer::RequireAggregation(common::ManagedPointer<parser::SelectStatement> op) {
  if (op->GetSelectGroupBy() != nullptr) {
    return true;
  }
  // Check plain aggregation
  bool has_aggregation = false;
  bool has_other_exprs = false;

  for (auto &expr : op->GetSelectColumns()) {
    std::vector<common::ManagedPointer<parser::AggregateExpression>> aggr_exprs;
    // we need to use get method of managed pointer because the function we are calling will recursivly get aggreate
    // expressions from the current expression and its children; children are of unique pointers
    parser::ExpressionUtil::GetAggregateExprs(&aggr_exprs, expr);
    if (!aggr_exprs.empty())
      has_aggregation = true;
    else
      has_other_exprs = true;
  }
  // TODO(peloton): Should be handled in the binder
  // Syntax error when there are mixture of aggregation and other exprs when group by is absent
  if (has_aggregation && has_other_exprs) {
    throw OPTIMIZER_EXCEPTION(
        "Non aggregation expression must appear in the GROUP BY clause or be used in an aggregate function");
  }
  return has_aggregation;
}

void QueryToOperatorTransformer::CollectPredicates(common::ManagedPointer<parser::AbstractExpression> expr,
                                                   std::vector<AnnotatedExpression> *predicates) {
  // First check if all conjunctive predicates are supported before transforming
  // predicate with sub-select into regular predicates
  std::vector<common::ManagedPointer<parser::AbstractExpression>> predicate_ptrs;
  QueryToOperatorTransformer::SplitPredicates(expr, &predicate_ptrs);
  for (const auto &pred : predicate_ptrs) {
    if (!QueryToOperatorTransformer::IsSupportedConjunctivePredicate(pred)) {
      throw NOT_IMPLEMENTED_EXCEPTION(
          ("Expression type " + std::to_string(static_cast<int>(pred.Get()->GetExpressionType())) + " is not supported")
              .c_str());
    }
  }
  // Accept will change the expression, e.g. (a in (select b from test)) into
  // (a IN test.b), after the rewrite, we can extract the table aliases
  // information correctly
  expr->Accept(common::ManagedPointer(this).CastManagedPointerTo<SqlNodeVisitor>());
  QueryToOperatorTransformer::ExtractPredicates(expr, predicates);
}

bool QueryToOperatorTransformer::IsSupportedConjunctivePredicate(
    common::ManagedPointer<parser::AbstractExpression> expr) {
  // Currently support : 1. expr without subquery
  // 2. subquery without disjunction. Since the expr is already one of the
  // conjunctive exprs, we'll only need to check if the root level is an
  // operator with subquery
  if (!expr->HasSubquery()) {
    return true;
  }
  auto expr_type = expr->GetExpressionType();
  // Subquery with IN
  if (expr_type == parser::ExpressionType::COMPARE_IN &&
      expr->GetChild(0)->GetExpressionType() != parser::ExpressionType::ROW_SUBQUERY &&
      expr->GetChild(1)->GetExpressionType() == parser::ExpressionType::ROW_SUBQUERY) {
    return true;
  }
  // Subquery with EXIST
  if (expr_type == parser::ExpressionType::OPERATOR_EXISTS &&
      expr->GetChild(0)->GetExpressionType() == parser::ExpressionType::ROW_SUBQUERY) {
    return true;
  }
  // Subquery with other operator
  if (expr_type == parser::ExpressionType::COMPARE_EQUAL || expr_type == parser::ExpressionType::COMPARE_GREATER_THAN ||
      expr_type == parser::ExpressionType::COMPARE_GREATER_THAN_OR_EQUAL_TO ||
      expr_type == parser::ExpressionType::COMPARE_LESS_THAN ||
      expr_type == parser::ExpressionType::COMPARE_LESS_THAN_OR_EQUAL_TO ||
      expr_type == parser::ExpressionType::COMPARE_NOT_EQUAL) {
    // Supported if one child is subquery and the other is not
    if ((!expr->GetChild(0)->HasSubquery() &&
         expr->GetChild(1)->GetExpressionType() == parser::ExpressionType::ROW_SUBQUERY) ||
        (!expr->GetChild(1)->HasSubquery() &&
         expr->GetChild(0)->GetExpressionType() == parser::ExpressionType::ROW_SUBQUERY)) {
      return true;
    }
  }
  return false;
}

bool QueryToOperatorTransformer::IsSupportedSubSelect(common::ManagedPointer<parser::SelectStatement> op) {
  // Supported if 1. No aggregation. 2. With aggregation and WHERE clause only
  // have correlated columns in conjunctive predicates in the form of
  // "outer_relation.a = ..."
  // TODO(boweic): Add support for arbitary expressions, this would require
  //  the support for mark join & some special operators, see Hyper's unnesting arbitary query slides
  if (!QueryToOperatorTransformer::RequireAggregation(op)) return true;

  std::vector<common::ManagedPointer<parser::AbstractExpression>> predicates;
  QueryToOperatorTransformer::SplitPredicates(op->GetSelectCondition(), &predicates);
  for (const auto &pred : predicates) {
    // Depth is used to detect correlated subquery, it is set in the binder,
    // if a TVE has depth less than the depth of the current operator, then it is correlated predicate
    if (pred->GetDepth() < op->GetDepth()) {
      if (pred->GetExpressionType() != parser::ExpressionType::COMPARE_EQUAL) {
        return false;
      }
      // Check if in the form of "outer_relation.a = (expr with only columns in inner relation)"
      if (!((pred->GetChild(1)->GetDepth() == op->GetDepth() &&
             pred->GetChild(0)->GetExpressionType() == parser::ExpressionType::COLUMN_VALUE) ||
            (pred->GetChild(0)->GetDepth() == op->GetDepth() &&
             pred->GetChild(1)->GetExpressionType() == parser::ExpressionType::COLUMN_VALUE))) {
        return false;
      }
    }
  }
  return true;
}

bool QueryToOperatorTransformer::GenerateSubqueryTree(common::ManagedPointer<parser::AbstractExpression> expr,
                                                      int child_id, bool single_join) {
  // Get potential subquery
  auto subquery_expr = expr->GetChild(child_id);
  if (subquery_expr->GetExpressionType() != parser::ExpressionType::ROW_SUBQUERY) return false;

  auto sub_select = subquery_expr.CastManagedPointerTo<parser::SubqueryExpression>()->GetSubselect();
  if (!QueryToOperatorTransformer::IsSupportedSubSelect(sub_select))
    throw NOT_IMPLEMENTED_EXCEPTION("Sub-select not supported");
  // We only support subselect with single row
  if (sub_select->GetSelectColumns().size() != 1) throw NOT_IMPLEMENTED_EXCEPTION("Array in predicates not supported");

  transaction::TransactionContext *txn_context = accessor_->GetTxn().Get();
  std::vector<parser::AbstractExpression *> select_list;
  // Construct join
  std::unique_ptr<OperatorNode> op_expr;
  if (single_join) {
    op_expr = std::make_unique<OperatorNode>(LogicalSingleJoin::Make().RegisterWithTxnContext(txn_context),
                                             std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
    op_expr->PushChild(std::move(output_expr_));
  } else {
    op_expr = std::make_unique<OperatorNode>(LogicalMarkJoin::Make().RegisterWithTxnContext(txn_context),
                                             std::vector<std::unique_ptr<AbstractOptimizerNode>>{}, txn_context);
    op_expr->PushChild(std::move(output_expr_));
  }

  sub_select->Accept(common::ManagedPointer(this).CastManagedPointerTo<SqlNodeVisitor>());

  // Push subquery output
  op_expr->PushChild(std::move(output_expr_));

  output_expr_ = std::move(op_expr);

  // Convert subquery to the selected column in the sub-select
  expr->SetChild(child_id, sub_select->GetSelectColumns().at(0));
  return true;
}

void QueryToOperatorTransformer::ExtractPredicates(common::ManagedPointer<parser::AbstractExpression> expr,
                                                   std::vector<AnnotatedExpression> *annotated_predicates) {
  // Split a complex predicate into a set of predicates connected by AND.
  std::vector<common::ManagedPointer<parser::AbstractExpression>> predicates;
  QueryToOperatorTransformer::SplitPredicates(expr, &predicates);

  for (auto predicate : predicates) {
    std::unordered_set<parser::AliasType> table_alias_set;
    QueryToOperatorTransformer::GenerateTableAliasSet(predicate, &table_alias_set);

    // Deep copy expression to avoid memory leak
    annotated_predicates->push_back(AnnotatedExpression(predicate, std::move(table_alias_set)));
  }
}

void QueryToOperatorTransformer::GenerateTableAliasSet(const common::ManagedPointer<parser::AbstractExpression> expr,
                                                       std::unordered_set<parser::AliasType> *table_alias_set) {
  if (expr->GetExpressionType() == parser::ExpressionType::COLUMN_VALUE) {
    table_alias_set->insert(expr.CastManagedPointerTo<const parser::ColumnValueExpression>()->GetTableAlias());
  } else {
    for (const auto &child : expr->GetChildren())
      QueryToOperatorTransformer::GenerateTableAliasSet(child, table_alias_set);
  }
}

void QueryToOperatorTransformer::SplitPredicates(
    common::ManagedPointer<parser::AbstractExpression> expr,
    std::vector<common::ManagedPointer<parser::AbstractExpression>> *predicates) {
  if (expr == nullptr) {
    return;
  }

  if (expr->GetExpressionType() == parser::ExpressionType::CONJUNCTION_AND) {
    // Traverse down the expression tree along conjunction
    for (const auto &child : expr->GetChildren()) {
      QueryToOperatorTransformer::SplitPredicates(common::ManagedPointer(child), predicates);
    }
  } else {
    // Find an expression that is the child of conjunction expression
    predicates->push_back(expr);
  }
}

std::unordered_map<parser::AliasType, common::ManagedPointer<parser::AbstractExpression>>
QueryToOperatorTransformer::ConstructSelectElementMap(
    const std::vector<common::ManagedPointer<parser::AbstractExpression>> &select_list) {
  std::unordered_map<parser::AliasType, common::ManagedPointer<parser::AbstractExpression>> res{};
  for (const auto &expr : select_list) {
    parser::AliasType alias;
    if (!expr->GetAlias().Empty()) {
      alias = expr->GetAlias();
    } else if (expr->GetExpressionType() == parser::ExpressionType::COLUMN_VALUE) {
      auto tv_expr = expr.CastManagedPointerTo<parser::ColumnValueExpression>();
      alias = parser::AliasType(tv_expr->GetColumnName());
    } else {
      continue;
    }
    res[alias] = common::ManagedPointer<parser::AbstractExpression>(expr);
  }
  return res;
}

void QueryToOperatorTransformer::ValidateInsertValues(
    common::ManagedPointer<parser::InsertStatement> insert_op,
    const std::vector<common::ManagedPointer<parser::AbstractExpression>> &values,
    catalog::table_oid_t target_table_id) {
  // column_objects represents the columns for the current table as defined in its schema
  auto column_objects = accessor_->GetSchema(target_table_id).GetColumns();

  // INSERT INTO table_name ...
  if (insert_op->GetInsertColumns()->empty()) {
    if (values.size() > column_objects.size()) {
      throw CATALOG_EXCEPTION("INSERT has more expressions than target columns");
    }
    if (values.size() < column_objects.size()) {
      for (auto i = values.size(); i != column_objects.size(); ++i) {
        // check whether null values or default values can be used in the rest of the columns
        if (!column_objects[i].Nullable() && column_objects[i].StoredExpression() == nullptr) {
          throw CATALOG_EXCEPTION(
              ("Null value in column \"" + column_objects[i].Name() + "\" violates not-null constraint").c_str());
        }
      }
    }
  } else {
    // INSERT INTO table_name (col1, col2, ...) ...
    auto num_columns = insert_op->GetInsertColumns()->size();

    if (values.size() > num_columns) {
      throw CATALOG_EXCEPTION("INSERT has more expressions than target columns");
    }
    if (values.size() < num_columns) {
      throw CATALOG_EXCEPTION("INSERT has more target columns than expressions");
    }
  }
}

}  // namespace noisepage::optimizer
