#include "optimizer/util.h"

#include <string>
#include <unordered_set>
#include <vector>

#include "catalog/catalog_accessor.h"
#include "optimizer/optimizer_defs.h"
#include "parser/expression/star_expression.h"
#include "parser/expression_util.h"

namespace noisepage::optimizer {

void OptimizerUtil::ExtractEquiJoinKeys(const std::vector<AnnotatedExpression> &join_predicates,
                                        std::vector<common::ManagedPointer<parser::AbstractExpression>> *left_keys,
                                        std::vector<common::ManagedPointer<parser::AbstractExpression>> *right_keys,
                                        const std::unordered_set<parser::AliasType> &left_alias,
                                        const std::unordered_set<parser::AliasType> &right_alias) {
  for (auto &expr_unit : join_predicates) {
    auto expr = expr_unit.GetExpr();
    if (expr->GetExpressionType() == parser::ExpressionType::COMPARE_EQUAL) {
      auto l_expr = expr->GetChild(0);
      auto r_expr = expr->GetChild(1);
      NOISEPAGE_ASSERT(l_expr->GetExpressionType() != parser::ExpressionType::VALUE_TUPLE &&
                           r_expr->GetExpressionType() != parser::ExpressionType::VALUE_TUPLE,
                       "DerivedValue should not exist here");

      // equi-join between two ColumnValueExpressions
      if (l_expr->GetExpressionType() == parser::ExpressionType::COLUMN_VALUE &&
          r_expr->GetExpressionType() == parser::ExpressionType::COLUMN_VALUE) {
        auto l_tv_expr = l_expr.CastManagedPointerTo<parser::ColumnValueExpression>();
        auto r_tv_expr = r_expr.CastManagedPointerTo<parser::ColumnValueExpression>();

        // Assign keys based on left and right join tables
        if (left_alias.find(l_tv_expr->GetTableAlias()) != left_alias.end() &&
            right_alias.find(r_tv_expr->GetTableAlias()) != right_alias.end()) {
          left_keys->emplace_back(l_expr);
          right_keys->emplace_back(r_expr);
        } else if (left_alias.find(r_tv_expr->GetTableAlias()) != left_alias.end() &&
                   right_alias.find(l_tv_expr->GetTableAlias()) != right_alias.end()) {
          left_keys->emplace_back(r_expr);
          right_keys->emplace_back(l_expr);
        }
      }
    }
  }
}

std::vector<parser::AbstractExpression *> OptimizerUtil::GenerateTableColumnValueExprs(
    catalog::CatalogAccessor *accessor, const parser::AliasType &alias, catalog::db_oid_t db_oid,
    catalog::table_oid_t tbl_oid) {
  // @note(boweic): we seems to provide all columns here, in case where there are
  // a lot of attributes and we're only visiting a few this is not efficient
  auto &schema = accessor->GetSchema(tbl_oid);
  auto &columns = schema.GetColumns();
  std::vector<parser::AbstractExpression *> exprs;
  for (auto &column : columns) {
    auto col_expr = GenerateColumnValueExpr(column, alias, db_oid, tbl_oid);
    exprs.push_back(col_expr);
  }

  return exprs;
}

parser::AbstractExpression *OptimizerUtil::GenerateColumnValueExpr(const catalog::Schema::Column &column,
                                                                   const parser::AliasType &alias,
                                                                   catalog::db_oid_t db_oid,
                                                                   catalog::table_oid_t tbl_oid) {
  auto col_oid = column.Oid();
  auto *col_expr = new parser::ColumnValueExpression(alias, column.Name());
  col_expr->SetReturnValueType(column.Type());
  col_expr->SetDatabaseOID(db_oid);
  col_expr->SetTableOID(tbl_oid);
  col_expr->SetColumnOID(col_oid);

  col_expr->DeriveExpressionName();
  col_expr->DeriveReturnValueType();
  return col_expr;
}

parser::AbstractExpression *OptimizerUtil::GenerateAggregateExpr(const catalog::Schema::Column &column,
                                                                 parser::ExpressionType aggregate_type, bool distinct,
                                                                 const parser::AliasType &alias,
                                                                 catalog::db_oid_t db_oid,
                                                                 catalog::table_oid_t tbl_oid) {
  auto col_expr = std::unique_ptr<parser::AbstractExpression>(GenerateColumnValueExpr(column, alias, db_oid, tbl_oid));
  std::vector<std::unique_ptr<parser::AbstractExpression>> agg_child;
  agg_child.emplace_back(std::move(col_expr));
  return new parser::AggregateExpression(aggregate_type, std::move(agg_child), distinct);
}

parser::AbstractExpression *OptimizerUtil::GenerateStarAggregateExpr(parser::ExpressionType aggregate_type,
                                                                     bool distinct) {
  auto star_expr = std::make_unique<parser::StarExpression>();
  std::vector<std::unique_ptr<parser::AbstractExpression>> agg_child;
  agg_child.emplace_back(std::move(star_expr));
  return new parser::AggregateExpression(aggregate_type, std::move(agg_child), distinct);
}

}  // namespace noisepage::optimizer
