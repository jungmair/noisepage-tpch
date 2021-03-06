#include "traffic_cop/traffic_cop.h"

#include <future>  // NOLINT
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "binder/bind_node_visitor.h"
#include "binder/binder_util.h"
#include "catalog/catalog.h"
#include "catalog/catalog_accessor.h"
#include "common/error/error_data.h"
#include "common/error/exception.h"
#include "common/thread_context.h"
#include "execution/compiler/compilation_context.h"
#include "execution/exec/execution_context.h"
#include "execution/exec/execution_settings.h"
#include "execution/exec/output.h"
#include "execution/sql/ddl_executors.h"
#include "execution/sql/value.h"
#include "execution/vm/module.h"
#include "metrics/metrics_store.h"
#include "network/connection_context.h"
#include "network/postgres/portal.h"
#include "network/postgres/postgres_packet_writer.h"
#include "network/postgres/statement.h"
#include "optimizer/cost_model/trivial_cost_model.h"
#include "optimizer/statistics/stats_storage.h"
#include "parser/drop_statement.h"
#include "parser/explain_statement.h"
#include "parser/expression/constant_value_expression.h"
#include "parser/postgresparser.h"
#include "parser/variable_set_statement.h"
#include "parser/variable_show_statement.h"
#include "planner/plannodes/abstract_plan_node.h"
#include "planner/plannodes/analyze_plan_node.h"
#include "planner/plannodes/create_database_plan_node.h"
#include "planner/plannodes/create_index_plan_node.h"
#include "planner/plannodes/create_namespace_plan_node.h"
#include "planner/plannodes/create_table_plan_node.h"
#include "planner/plannodes/drop_database_plan_node.h"
#include "planner/plannodes/drop_index_plan_node.h"
#include "planner/plannodes/drop_namespace_plan_node.h"
#include "planner/plannodes/drop_table_plan_node.h"
#include "settings/settings_manager.h"
#include "settings/settings_param.h"
#include "storage/recovery/replication_log_provider.h"
#include "traffic_cop/traffic_cop_defs.h"
#include "traffic_cop/traffic_cop_util.h"
#include "transaction/transaction_manager.h"

namespace noisepage::trafficcop {

/** The commit callback argument. */
struct CommitCallbackArg {
  std::atomic<uint8_t> persist_countdown_;  ///< A countdown latch for what else needs to persist.
  std::promise<bool> ready_to_commit_;      ///< Set this promise to true to wake up the thread for commit.

  explicit CommitCallbackArg(const transaction::TransactionPolicy &policy) {
    // The value for the persist_countdown_ field.
    // Note that the field will be decremented exactly once every time a commit callback is invoked.
    persist_countdown_ = 0;

    // Cases: Durability, Replication
    // - DISABLE, DISABLE => 1. The callback is invoked on LogCommit in TransactionManager.
    // - ASYNC, SYNC => This is too weird. Not supporting this.
    // - ASYNC, ASYNC => 1. The callback is invoked immediately in TransactionManager.
    // - SYNC, ASYNC => 2. The callback is invoked by DiskLogConsumerTask and PrimaryReplicationManager.
    // - SYNC, SYNC => 2. The callback is invoked by DiskLogConsumerTask and PrimaryReplicationManager.

    NOISEPAGE_ASSERT(!(policy.durability_ == transaction::DurabilityPolicy::ASYNC &&
                       policy.replication_ == transaction::ReplicationPolicy::SYNC),
                     "Haven't reasoned about this case.");

    const transaction::DurabilityPolicy &dur = policy.durability_;
    const transaction::ReplicationPolicy &rep = policy.replication_;

    // Commit callback is always invoked at least once.
    persist_countdown_ += 1;
    if (rep != transaction::ReplicationPolicy::DISABLE) {
      if (dur == transaction::DurabilityPolicy::ASYNC && rep == transaction::ReplicationPolicy::ASYNC) {
        // Callback will get invoked by TransactionManager, fake EmptyCallback is passed down.
      } else {
        NOISEPAGE_ASSERT(dur != transaction::DurabilityPolicy::DISABLE, "Nothing to replicate?");
        NOISEPAGE_ASSERT(dur == transaction::DurabilityPolicy::SYNC, "What other policies are there?");
        persist_countdown_ += 1;
      }
    }
  }
};

static void CommitCallback(void *const callback_arg) {
  auto *const cb_arg = reinterpret_cast<CommitCallbackArg *const>(callback_arg);
  const uint8_t count_before_sub = cb_arg->persist_countdown_.fetch_sub(1);
  NOISEPAGE_ASSERT(
      count_before_sub != 0,
      "Every component should have invoked the callback already. The policy may not have been correctly initialized?");
  const bool was_last_callback = count_before_sub == 1;
  if (was_last_callback) {
    cb_arg->ready_to_commit_.set_value(true);
  }
}

void TrafficCop::BeginTransaction(const common::ManagedPointer<network::ConnectionContext> connection_ctx) const {
  NOISEPAGE_ASSERT(connection_ctx->TransactionState() == network::NetworkTransactionStateType::IDLE,
                   "Invalid ConnectionContext state, already in a transaction.");
  const auto txn = txn_manager_->BeginTransaction();
  connection_ctx->SetTransaction(common::ManagedPointer(txn));
  connection_ctx->SetAccessor(catalog_->GetAccessor(common::ManagedPointer(txn), connection_ctx->GetDatabaseOid(),
                                                    connection_ctx->GetCatalogCache()));
}

void TrafficCop::EndTransaction(const common::ManagedPointer<network::ConnectionContext> connection_ctx,
                                const network::QueryType query_type) const {
  NOISEPAGE_ASSERT(query_type == network::QueryType::QUERY_COMMIT || query_type == network::QueryType::QUERY_ROLLBACK,
                   "EndTransaction called with invalid QueryType.");
  const auto txn = connection_ctx->Transaction();
  if (query_type == network::QueryType::QUERY_COMMIT) {
    NOISEPAGE_ASSERT(connection_ctx->TransactionState() == network::NetworkTransactionStateType::BLOCK,
                     "Invalid ConnectionContext state, not in a transaction that can be committed.");
    // Set up a blocking callback. Will be invoked when we can tell the client that commit is complete.
    CommitCallbackArg cb_arg(txn->GetTransactionPolicy());
    auto future = cb_arg.ready_to_commit_.get_future();
    NOISEPAGE_ASSERT(future.valid(), "future must be valid for synchronization to work.");
    txn_manager_->Commit(txn.Get(), CommitCallback, &cb_arg);
    future.wait();
    NOISEPAGE_ASSERT(future.get(), "Got past the wait() without the value being set to true. That's weird.");
  } else {
    NOISEPAGE_ASSERT(connection_ctx->TransactionState() != network::NetworkTransactionStateType::IDLE,
                     "Invalid ConnectionContext state, not in a transaction that can be aborted.");
    txn_manager_->Abort(txn.Get());
  }
  connection_ctx->SetTransaction(nullptr);
  connection_ctx->SetAccessor(nullptr);
}

void TrafficCop::ExecuteTransactionStatement(const common::ManagedPointer<network::ConnectionContext> connection_ctx,
                                             const common::ManagedPointer<network::PostgresPacketWriter> out,
                                             const bool explicit_txn_block,
                                             const noisepage::network::QueryType query_type) const {
  NOISEPAGE_ASSERT(query_type == network::QueryType::QUERY_COMMIT || query_type == network::QueryType::QUERY_ROLLBACK ||
                       query_type == network::QueryType::QUERY_BEGIN,
                   "ExecuteTransactionStatement called with invalid QueryType.");
  switch (query_type) {
    case network::QueryType::QUERY_BEGIN: {
      NOISEPAGE_ASSERT(connection_ctx->TransactionState() != network::NetworkTransactionStateType::FAIL,
                       "We're in an aborted state. This should have been caught already before calling this function.");
      if (explicit_txn_block) {
        out->WriteError({common::ErrorSeverity::WARNING, "there is already a transaction in progress",
                         common::ErrorCode::ERRCODE_ACTIVE_SQL_TRANSACTION});
        break;
      }
      break;
    }
    case network::QueryType::QUERY_COMMIT: {
      if (!explicit_txn_block) {
        out->WriteError({common::ErrorSeverity::WARNING, "there is no transaction in progress",
                         common::ErrorCode::ERRCODE_NO_ACTIVE_SQL_TRANSACTION});
        break;
      }
      if (connection_ctx->TransactionState() == network::NetworkTransactionStateType::FAIL) {
        EndTransaction(connection_ctx, network::QueryType::QUERY_ROLLBACK);
        out->WriteCommandComplete(network::QueryType::QUERY_ROLLBACK, 0);
        return;
      }
      EndTransaction(connection_ctx, network::QueryType::QUERY_COMMIT);
      break;
    }
    case network::QueryType::QUERY_ROLLBACK: {
      if (!explicit_txn_block) {
        out->WriteError({common::ErrorSeverity::WARNING, "there is no transaction in progress",
                         common::ErrorCode::ERRCODE_NO_ACTIVE_SQL_TRANSACTION});
        break;
      }
      EndTransaction(connection_ctx, network::QueryType::QUERY_ROLLBACK);
      break;
    }
    default:
      UNREACHABLE("ExecuteTransactionStatement called with invalid QueryType.");
  }
  out->WriteCommandComplete(query_type, 0);
}

std::unique_ptr<optimizer::OptimizeResult> TrafficCop::OptimizeBoundQuery(
    const common::ManagedPointer<network::ConnectionContext> connection_ctx,
    const common::ManagedPointer<parser::ParseResult> query,
    common::ManagedPointer<std::vector<parser::ConstantValueExpression>> parameters) const {
  NOISEPAGE_ASSERT(connection_ctx->TransactionState() == network::NetworkTransactionStateType::BLOCK,
                   "Not in a valid txn. This should have been caught before calling this function.");

  return TrafficCopUtil::Optimize(connection_ctx->Transaction(), connection_ctx->Accessor(), query,
                                  connection_ctx->GetDatabaseOid(), stats_storage_,
                                  std::make_unique<optimizer::TrivialCostModel>(), optimizer_timeout_, parameters);
}

TrafficCopResult TrafficCop::ExecuteSetStatement(common::ManagedPointer<network::ConnectionContext> connection_ctx,
                                                 common::ManagedPointer<network::Statement> statement) const {
  NOISEPAGE_ASSERT(connection_ctx->TransactionState() == network::NetworkTransactionStateType::IDLE,
                   "This is a non-transactional operation and we should not be in a transaction.");
  NOISEPAGE_ASSERT(statement->GetQueryType() == network::QueryType::QUERY_SET,
                   "ExecuteSetStatement called with invalid QueryType.");

  const auto &set_stmt = statement->RootStatement().CastManagedPointerTo<parser::VariableSetStatement>();

  try {
    if (set_stmt->IsSetDefault()) {
      // TODO(WAN): Annoyingly, a copy is done for default_val because of differences in const qualifiers.
      parser::ConstantValueExpression default_val = settings_manager_->GetDefault(set_stmt->GetParameterName());
      auto default_val_ptr = common::ManagedPointer(&default_val).CastManagedPointerTo<parser::AbstractExpression>();
      settings_manager_->SetParameter(set_stmt->GetParameterName(), {default_val_ptr});
    } else {
      settings_manager_->SetParameter(set_stmt->GetParameterName(), set_stmt->GetValues());
    }
  } catch (SettingsException &e) {
    auto error = common::ErrorData(common::ErrorSeverity::ERROR, e.what(), e.code_);
    error.AddField(common::ErrorField::LINE, std::to_string(e.GetLine()));
    error.AddField(common::ErrorField::FILE, e.GetFile());
    return {ResultType::ERROR, error};
  }

  return {ResultType::COMPLETE, 0u};
}

TrafficCopResult TrafficCop::ExecuteShowStatement(common::ManagedPointer<network::ConnectionContext> connection_ctx,
                                                  common::ManagedPointer<network::PostgresPacketWriter> out,
                                                  common::ManagedPointer<network::Statement> statement) const {
  NOISEPAGE_ASSERT(connection_ctx->TransactionState() == network::NetworkTransactionStateType::IDLE,
                   "This is a non-transactional operation and we should not be in a transaction.");
  NOISEPAGE_ASSERT(statement->GetQueryType() == network::QueryType::QUERY_SHOW,
                   "ExecuteSetStatement called with invalid QueryType.");

  const auto &show_stmt UNUSED_ATTRIBUTE =
      statement->RootStatement().CastManagedPointerTo<parser::VariableShowStatement>();

  const std::string &param_name = show_stmt->GetName();
  settings::Param param = settings_manager_->GetParam(param_name);
  const settings::ParamInfo &param_info = settings_manager_->GetParamInfo(param);
  std::string param_val = param_info.GetValue().ToString();

  auto expr = std::make_unique<parser::ConstantValueExpression>(execution::sql::SqlTypeId::Varchar);
  expr->SetAlias(parser::AliasType(param_name));
  std::vector<noisepage::planner::OutputSchema::Column> cols;
  cols.emplace_back(param_name, execution::sql::SqlTypeId::Varchar, std::move(expr));
  execution::sql::StringVal result{param_val.c_str()};

  out->WriteDataRow(reinterpret_cast<const byte *>(&result), cols, {network::FieldFormat::text});
  return {ResultType::COMPLETE, 0u};
}

TrafficCopResult TrafficCop::ExecuteCreateStatement(
    const common::ManagedPointer<network::ConnectionContext> connection_ctx,
    const common::ManagedPointer<planner::AbstractPlanNode> physical_plan,
    const noisepage::network::QueryType query_type) const {
  NOISEPAGE_ASSERT(connection_ctx->TransactionState() == network::NetworkTransactionStateType::BLOCK,
                   "Not in a valid txn. This should have been caught before calling this function.");
  NOISEPAGE_ASSERT(
      query_type == network::QueryType::QUERY_CREATE_TABLE || query_type == network::QueryType::QUERY_CREATE_SCHEMA ||
          query_type == network::QueryType::QUERY_CREATE_INDEX || query_type == network::QueryType::QUERY_CREATE_DB ||
          query_type == network::QueryType::QUERY_CREATE_VIEW || query_type == network::QueryType::QUERY_CREATE_TRIGGER,
      "ExecuteCreateStatement called with invalid QueryType.");
  switch (query_type) {
    case network::QueryType::QUERY_CREATE_TABLE: {
      if (execution::sql::DDLExecutors::CreateTableExecutor(
              physical_plan.CastManagedPointerTo<planner::CreateTablePlanNode>(), connection_ctx->Accessor(),
              connection_ctx->GetDatabaseOid())) {
        return {ResultType::COMPLETE, 0u};
      }
      break;
    }
    case network::QueryType::QUERY_CREATE_DB: {
      if (execution::sql::DDLExecutors::CreateDatabaseExecutor(
              physical_plan.CastManagedPointerTo<planner::CreateDatabasePlanNode>(), connection_ctx->Accessor())) {
        return {ResultType::COMPLETE, 0u};
      }
      break;
    }
    case network::QueryType::QUERY_CREATE_INDEX: {
      if (execution::sql::DDLExecutors::CreateIndexExecutor(
              physical_plan.CastManagedPointerTo<planner::CreateIndexPlanNode>(), connection_ctx->Accessor())) {
        return {ResultType::COMPLETE, 0u};
      }
      break;
    }
    case network::QueryType::QUERY_CREATE_SCHEMA: {
      if (execution::sql::DDLExecutors::CreateNamespaceExecutor(
              physical_plan.CastManagedPointerTo<planner::CreateNamespacePlanNode>(), connection_ctx->Accessor())) {
        return {ResultType::COMPLETE, 0u};
      }
      break;
    }
    default: {
      return {ResultType::ERROR, common::ErrorData(common::ErrorSeverity::ERROR, "unsupported CREATE statement type",
                                                   common::ErrorCode::ERRCODE_FEATURE_NOT_SUPPORTED)};
    }
  }
  connection_ctx->Transaction()->SetMustAbort();
  // TODO(Matt): get more verbose failure info out of the catalog and DDL executors. I think at this point, if it's past
  // binding, the only thing you could fail on is a conflict with a DDL change you already made in this txn or failure
  // to acquire DDL lock?
  return {ResultType::ERROR, common::ErrorData(common::ErrorSeverity::ERROR, "failed to execute CREATE",
                                               common::ErrorCode::ERRCODE_DATA_EXCEPTION)};
}

TrafficCopResult TrafficCop::ExecuteDropStatement(
    const common::ManagedPointer<network::ConnectionContext> connection_ctx,
    const common::ManagedPointer<planner::AbstractPlanNode> physical_plan,
    const noisepage::network::QueryType query_type) const {
  NOISEPAGE_ASSERT(connection_ctx->TransactionState() == network::NetworkTransactionStateType::BLOCK,
                   "Not in a valid txn. This should have been caught before calling this function.");
  NOISEPAGE_ASSERT(
      query_type == network::QueryType::QUERY_DROP_TABLE || query_type == network::QueryType::QUERY_DROP_SCHEMA ||
          query_type == network::QueryType::QUERY_DROP_INDEX || query_type == network::QueryType::QUERY_DROP_DB ||
          query_type == network::QueryType::QUERY_DROP_VIEW || query_type == network::QueryType::QUERY_DROP_TRIGGER,
      "ExecuteDropStatement called with invalid QueryType.");
  switch (query_type) {
    case network::QueryType::QUERY_DROP_TABLE: {
      if (execution::sql::DDLExecutors::DropTableExecutor(
              physical_plan.CastManagedPointerTo<planner::DropTablePlanNode>(), connection_ctx->Accessor())) {
        return {ResultType::COMPLETE, 0u};
      }
      break;
    }
    case network::QueryType::QUERY_DROP_DB: {
      if (execution::sql::DDLExecutors::DropDatabaseExecutor(
              physical_plan.CastManagedPointerTo<planner::DropDatabasePlanNode>(), connection_ctx->Accessor(),
              connection_ctx->GetDatabaseOid())) {
        return {ResultType::COMPLETE, 0u};
      }
      break;
    }
    case network::QueryType::QUERY_DROP_INDEX: {
      if (execution::sql::DDLExecutors::DropIndexExecutor(
              physical_plan.CastManagedPointerTo<planner::DropIndexPlanNode>(), connection_ctx->Accessor())) {
        return {ResultType::COMPLETE, 0u};
      }
      break;
    }
    case network::QueryType::QUERY_DROP_SCHEMA: {
      if (execution::sql::DDLExecutors::DropNamespaceExecutor(
              physical_plan.CastManagedPointerTo<planner::DropNamespacePlanNode>(), connection_ctx->Accessor())) {
        return {ResultType::COMPLETE, 0u};
      }
      break;
    }
    default: {
      return {ResultType::ERROR, common::ErrorData(common::ErrorSeverity::ERROR, "unsupported DROP statement type",
                                                   common::ErrorCode::ERRCODE_FEATURE_NOT_SUPPORTED)};
    }
  }
  connection_ctx->Transaction()->SetMustAbort();
  // TODO(Matt): get more verbose failure info out of the catalog and DDL executors. I think at this point, if it's past
  // binding, the only thing you could fail on is a conflict with a DDL change you already made in this txn or failure
  // to acquire DDL lock?
  return {ResultType::ERROR, common::ErrorData(common::ErrorSeverity::ERROR, "failed to execute DROP",
                                               common::ErrorCode::ERRCODE_DATA_EXCEPTION)};
}

TrafficCopResult TrafficCop::ExecuteExplainStatement(
    const common::ManagedPointer<network::ConnectionContext> connection_ctx,
    const common::ManagedPointer<network::PostgresPacketWriter> out,
    const common::ManagedPointer<network::Portal> portal) const {
  NOISEPAGE_ASSERT(connection_ctx->TransactionState() == network::NetworkTransactionStateType::BLOCK,
                   "Not in a valid txn. This should have been caught before calling this function.");

  // Dump to JSON string, wrap in StringVal, write the data row to the client
  // Create dummy column output scheme for writing data row
  std::vector<planner::OutputSchema::Column> output_columns;
  output_columns.emplace_back("QUERY PLAN", execution::sql::SqlTypeId::Varchar, nullptr);

  const auto format =
      portal->GetStatement()->RootStatement().CastManagedPointerTo<parser::ExplainStatement>()->GetFormat();
  std::string plan_string;
  if (format == parser::ExplainStatementFormat::JSON) {
    plan_string = portal->OptimizeResult()->GetPlanNode()->ToJson().dump(4);
  } else {
    NOISEPAGE_ASSERT(format == parser::ExplainStatementFormat::TPL || format == parser::ExplainStatementFormat::TBC,
                     "We only support JSON, TPL, and TBC formats.");

    // Codegen must happen for certain types of EXPLAIN metadata to be collected, e.g., collection of TPL.
    auto codegen = CodegenPhysicalPlan(connection_ctx, out, portal);
    if (codegen.type_ != ResultType::COMPLETE) {
      return {ResultType::ERROR, common::ErrorData(common::ErrorSeverity::ERROR, "Failed to execute codegen.",
                                                   common::ErrorCode::ERRCODE_DATA_EXCEPTION)};
    }

    const auto &fragments = portal->GetStatement()->GetExecutableQuery()->GetFragments();
    NOISEPAGE_ASSERT(fragments.size() == 1, "We currently always compile with just one query fragment.");
    const auto &metadata = fragments.at(0)->GetModuleMetadata().GetCompileTimeMetadata();

    if (format == parser::ExplainStatementFormat::TPL) {
      plan_string = metadata.GetTPL();
    } else {
      NOISEPAGE_ASSERT(format == parser::ExplainStatementFormat::TBC, "Did you add a new case?");
      plan_string = metadata.GetTBC();
    }
  }
  const auto plan_string_val = execution::sql::StringVal(plan_string.c_str(), plan_string.length());
  out->WriteDataRow(reinterpret_cast<const byte *const>(&plan_string_val), output_columns,
                    {network::FieldFormat::text});

  return {ResultType::COMPLETE, 0u};
}

std::variant<std::unique_ptr<parser::ParseResult>, common::ErrorData> TrafficCop::ParseQuery(
    const std::string &query, const common::ManagedPointer<network::ConnectionContext> connection_ctx) const {
  std::variant<std::unique_ptr<parser::ParseResult>, common::ErrorData> result;
  try {
    auto parse_result = parser::PostgresParser::BuildParseTree(query);
    result.emplace<std::unique_ptr<parser::ParseResult>>(std::move(parse_result));
  } catch (const ParserException &e) {
    common::ErrorData error(common::ErrorSeverity::ERROR, std::string(e.what()),
                            common::ErrorCode::ERRCODE_SYNTAX_ERROR);
    error.AddField(common::ErrorField::POSITION, std::to_string(e.GetCursorPos()));
    result.emplace<common::ErrorData>(std::move(error));
  }
  return result;
}

TrafficCopResult TrafficCop::BindQuery(
    const common::ManagedPointer<network::ConnectionContext> connection_ctx,
    const common::ManagedPointer<network::Statement> statement,
    const common::ManagedPointer<std::vector<parser::ConstantValueExpression>> parameters) const {
  NOISEPAGE_ASSERT(connection_ctx->TransactionState() == network::NetworkTransactionStateType::BLOCK,
                   "Not in a valid txn. This should have been caught before calling this function.");

  try {
    if (statement->OptimizeResult() == nullptr || !UseQueryCache()) {
      // it's not cached, bind it
      binder::BindNodeVisitor visitor(connection_ctx->Accessor(), connection_ctx->GetDatabaseOid());
      if (parameters != nullptr && !parameters->empty()) {
        std::vector<execution::sql::SqlTypeId> desired_param_types(
            parameters->size());  // default construction of values is fine, Binding will overwrite it
        visitor.BindNameToNode(statement->ParseResult(), parameters, common::ManagedPointer(&desired_param_types));
        statement->SetDesiredParamTypes(std::move(desired_param_types));
      } else {
        visitor.BindNameToNode(statement->ParseResult(), nullptr, nullptr);
      }
    } else {
      // it's cached. use the desired_param_types to fast-path the binding
      binder::BinderUtil::PromoteParameters(parameters, statement->GetDesiredParamTypes());
    }
  } catch (BinderException &e) {
    // Failed to bind
    // TODO(Matt): this is a hack to get IF EXISTS to work with our tests, we actually need better support in
    // PostgresParser and the binder should return more state back to the TrafficCop to figure out what to do
    if ((statement->RootStatement()->GetType() == parser::StatementType::DROP &&
         statement->RootStatement().CastManagedPointerTo<parser::DropStatement>()->IsIfExists())) {
      return {ResultType::NOTICE, common::ErrorData(common::ErrorSeverity::NOTICE,
                                                    "binding failed with an IF EXISTS clause, skipping statement",
                                                    common::ErrorCode::ERRCODE_SUCCESSFUL_COMPLETION)};
    }
    auto error = common::ErrorData(common::ErrorSeverity::ERROR, e.what(), e.code_);
    error.AddField(common::ErrorField::LINE, std::to_string(e.GetLine()));
    error.AddField(common::ErrorField::FILE, e.GetFile());
    return {ResultType::ERROR, error};
  }

  return {ResultType::COMPLETE, 0u};
}

TrafficCopResult TrafficCop::CodegenPhysicalPlan(
    const common::ManagedPointer<network::ConnectionContext> connection_ctx,
    const common::ManagedPointer<network::PostgresPacketWriter> out,
    const common::ManagedPointer<network::Portal> portal) const {
  NOISEPAGE_ASSERT(connection_ctx->TransactionState() == network::NetworkTransactionStateType::BLOCK,
                   "Not in a valid txn. This should have been caught before calling this function.");
  // For an EXPLAIN statement, the query type is the type of the wrapped SQL statement.
  const auto query_type UNUSED_ATTRIBUTE =
      portal->GetStatement()->GetQueryType() == network::QueryType::QUERY_EXPLAIN
          ? trafficcop::TrafficCopUtil::QueryTypeForStatement(portal->GetStatement()
                                                                  ->RootStatement()
                                                                  .CastManagedPointerTo<parser::ExplainStatement>()
                                                                  ->GetSQLStatement())
          : portal->GetStatement()->GetQueryType();
  const auto physical_plan = portal->OptimizeResult()->GetPlanNode();

  NOISEPAGE_ASSERT(
      query_type == network::QueryType::QUERY_SELECT || query_type == network::QueryType::QUERY_INSERT ||
          query_type == network::QueryType::QUERY_CREATE_INDEX || query_type == network::QueryType::QUERY_UPDATE ||
          query_type == network::QueryType::QUERY_DELETE || query_type == network::QueryType::QUERY_ANALYZE,
      "CodegenPhysicalPlan called with invalid QueryType.");

  if (portal->GetStatement()->GetExecutableQuery() != nullptr && use_query_cache_) {
    // We've already codegen'd this, move on...
    return {ResultType::COMPLETE, 0u};
  }

  // TODO(WAN): see #1047
  execution::exec::ExecutionSettings exec_settings{};
  exec_settings.UpdateFromSettingsManager(settings_manager_);

  // Set any compilation settings based on the original query type.
  if (portal->GetStatement()->GetQueryType() == network::QueryType::QUERY_EXPLAIN) {
    execution::compiler::CompilerSettings settings;
    auto stmt = portal->GetStatement()->RootStatement().CastManagedPointerTo<parser::ExplainStatement>();
    if (stmt->GetFormat() == parser::ExplainStatementFormat::TPL) {
      settings.SetShouldCaptureTPL(true);
    } else if (stmt->GetFormat() == parser::ExplainStatementFormat::TBC) {
      settings.SetShouldCaptureTBC(true);
    }
    exec_settings.SetCompilerSettings(settings);
  }

  auto exec_query = execution::compiler::CompilationContext::Compile(
      *physical_plan, exec_settings, connection_ctx->Accessor().Get(),
      execution::compiler::CompilationMode::Interleaved, std::nullopt, portal->OptimizeResult()->GetPlanMetaData());

  // TODO(Matt): handle code generation failing
  // Only record query text when generating the ExecutableQuery for the first time
  if (portal->GetStatement()->GetExecutableQuery() == nullptr) {
    const bool query_trace_metrics_enabled =
        common::thread_context.metrics_store_ != nullptr &&
        common::thread_context.metrics_store_->ComponentToRecord(metrics::MetricsComponent::QUERY_TRACE);
    if (query_trace_metrics_enabled) {
      common::thread_context.metrics_store_->RecordQueryText(connection_ctx->GetDatabaseOid(), exec_query->GetQueryId(),
                                                             portal->GetStatement()->GetQueryText(),
                                                             portal->Parameters(), metrics::MetricsUtil::Now());
    }
  }

  portal->GetStatement()->SetExecutableQuery(std::move(exec_query));

  return {ResultType::COMPLETE, 0u};
}

TrafficCopResult TrafficCop::RunExecutableQuery(const common::ManagedPointer<network::ConnectionContext> connection_ctx,
                                                const common::ManagedPointer<network::PostgresPacketWriter> out,
                                                const common::ManagedPointer<network::Portal> portal) const {
  NOISEPAGE_ASSERT(connection_ctx->TransactionState() == network::NetworkTransactionStateType::BLOCK,
                   "Not in a valid txn. This should have been caught before calling this function.");
  const auto query_type = portal->GetStatement()->GetQueryType();
  NOISEPAGE_ASSERT(
      query_type == network::QueryType::QUERY_SELECT || query_type == network::QueryType::QUERY_INSERT ||
          query_type == network::QueryType::QUERY_CREATE_INDEX || query_type == network::QueryType::QUERY_UPDATE ||
          query_type == network::QueryType::QUERY_DELETE || query_type == network::QueryType::QUERY_ANALYZE,
      "CodegenAndRunPhysicalPlan called with invalid QueryType.");

  if (query_cache_timestamp_ > portal->GetStatement()->GetExecutableQuery()->GetTimestamp()) {
    // ExecutableQuery is outdated. Re-generate it
    auto statement = portal->GetStatement();
    statement->SetExecutableQuery(nullptr);
    // Re-optimize the query (e.g., there can be new indexes that the query can use)
    auto optimize_result = OptimizeBoundQuery(connection_ctx, statement->ParseResult(), portal->ModifiableParameters());
    statement->SetOptimizeResult(std::move(optimize_result));

    CodegenPhysicalPlan(connection_ctx, out, portal);
  }

  const auto physical_plan = portal->OptimizeResult()->GetPlanNode();

  /*
   * ANALYZE will update the statistics held in the pg_statistic catalog table. These statistics are also cached in
   * StatsStorage. So once ANALYZE commits, we need to mark the columns updated as dirty in StatsStorage.
   */
  if (query_type == network::QueryType::QUERY_ANALYZE) {
    const auto analyze_plan = physical_plan.CastManagedPointerTo<planner::AnalyzePlanNode>();
    auto db_oid = analyze_plan->GetDatabaseOid();
    auto table_oid = analyze_plan->GetTableOid();
    std::vector<catalog::col_oid_t> col_oids = analyze_plan->GetColumnOids();
    connection_ctx->Transaction()->RegisterCommitAction(
        [=]() { stats_storage_->MarkStatsStale(db_oid, table_oid, col_oids); });
  }

  execution::exec::OutputWriter writer(physical_plan->GetOutputSchema(), out, portal->ResultFormats());

  // A std::function<> requires the target to be CopyConstructible and CopyAssignable. In certain
  // cases constructing a std::function<> copies the target. This can lead to cases where invoking
  // the std::function<> will call operator() on a OutputWriter different from the writer above.
  //
  // We utilize an extra lambda to capture a pointer to writer. This will allow all OutputBuffers
  // created during execution to write to the output consumer using the same writer instance
  // (which will also yield a correct writer.NumRows()).
  execution::exec::OutputWriter *capture_writer = &writer;
  execution::exec::OutputCallback callback = [capture_writer](byte *tuples, uint32_t num_tuples, uint32_t tuple_size) {
    (*capture_writer)(tuples, num_tuples, tuple_size);
  };

  execution::exec::ExecutionSettings exec_settings{};
  exec_settings.UpdateFromSettingsManager(settings_manager_);

  common::ManagedPointer<metrics::MetricsManager> metrics = nullptr;
  if (common::thread_context.metrics_store_ != nullptr) {
    metrics = common::thread_context.metrics_store_->MetricsManager();
  }

  auto exec_ctx = std::make_unique<execution::exec::ExecutionContext>(
      connection_ctx->GetDatabaseOid(), connection_ctx->Transaction(), callback, physical_plan->GetOutputSchema().Get(),
      connection_ctx->Accessor(), exec_settings, metrics, replication_manager_, recovery_manager_);

  exec_ctx->SetParams(portal->Parameters());

  const auto exec_query = portal->GetStatement()->GetExecutableQuery();

  try {
    exec_query->Run(common::ManagedPointer(exec_ctx), execution_mode_);
  } catch (ExecutionException &e) {
    /*
     * An ExecutionException is thrown in the case of some failure caused by a software bug or caused by some data
     * exception. In either case we abort the current transaction and return an error to the client.
     */
    // TODO(Matt): evict this plan from the statement cache when the functionality is available
    connection_ctx->Transaction()->SetMustAbort();
    auto error = common::ErrorData(common::ErrorSeverity::ERROR, e.what(), e.code_);
    error.AddField(common::ErrorField::LINE, std::to_string(e.GetLine()));
    error.AddField(common::ErrorField::FILE, e.GetFile());
    return {ResultType::ERROR, error};
  }

  const bool query_trace_metrics_enabled =
      common::thread_context.metrics_store_ != nullptr &&
      common::thread_context.metrics_store_->ComponentToRecord(metrics::MetricsComponent::QUERY_TRACE);

  if (query_trace_metrics_enabled) {
    common::thread_context.metrics_store_->RecordQueryTrace(connection_ctx->GetDatabaseOid(), exec_query->GetQueryId(),
                                                            metrics::MetricsUtil::Now(), portal->Parameters());
  }

  if (connection_ctx->TransactionState() == network::NetworkTransactionStateType::BLOCK) {
    // Execution didn't set us to FAIL state, go ahead and return command complete
    if (query_type == network::QueryType::QUERY_SELECT) {
      // For selects we rely on the OutputWriter to store the number of rows affected because sequential scan
      // iteration can happen in multiple pipelines
      return {ResultType::COMPLETE, writer.NumRows()};
    }
    // Other queries (INSERT, UPDATE, DELETE) retrieve rows affected from the execution context since other queries
    // might not have any output otherwise
    return {ResultType::COMPLETE, exec_ctx->GetRowsAffected()};
  }

  // TODO(Matt): We need a more verbose way to say what happened during execution (INSERT failed for key conflict,
  // etc.) I suspect we would stash that in the ExecutionContext.
  return {ResultType::ERROR, common::ErrorData(common::ErrorSeverity::ERROR, "Query failed.",
                                               common::ErrorCode::ERRCODE_T_R_SERIALIZATION_FAILURE)};
}

std::pair<catalog::db_oid_t, catalog::namespace_oid_t> TrafficCop::CreateTempNamespace(
    const network::connection_id_t connection_id, const std::string &database_name) {
  auto *const txn = txn_manager_->BeginTransaction();
  txn->SetReplicationPolicy(transaction::ReplicationPolicy::DISABLE);

  const auto db_oid = catalog_->GetDatabaseOid(common::ManagedPointer(txn), database_name);

  if (db_oid == catalog::INVALID_DATABASE_OID) {
    // Invalid database name
    txn_manager_->Abort(txn);
    return {catalog::INVALID_DATABASE_OID, catalog::INVALID_NAMESPACE_OID};
  }

  const auto ns_oid =
      catalog_->GetAccessor(common::ManagedPointer(txn), db_oid, DISABLED)
          ->CreateNamespace(std::string(TEMP_NAMESPACE_PREFIX) + std::to_string(connection_id.UnderlyingValue()));
  if (ns_oid == catalog::INVALID_NAMESPACE_OID) {
    // Failed to create new namespace. Could be a concurrent DDL change and worth retrying
    txn_manager_->Abort(txn);
    return {db_oid, catalog::INVALID_NAMESPACE_OID};
  }

  // Success
  txn_manager_->Commit(txn, transaction::TransactionUtil::EmptyCallback, nullptr);
  return {db_oid, ns_oid};
}

bool TrafficCop::DropTempNamespace(const catalog::db_oid_t db_oid, const catalog::namespace_oid_t ns_oid) {
  NOISEPAGE_ASSERT(db_oid != catalog::INVALID_DATABASE_OID, "Called DropTempNamespace() with an invalid database oid.");
  NOISEPAGE_ASSERT(ns_oid != catalog::INVALID_NAMESPACE_OID,
                   "Called DropTempNamespace() with an invalid namespace oid.");
  auto *const txn = txn_manager_->BeginTransaction();
  txn->SetReplicationPolicy(transaction::ReplicationPolicy::DISABLE);

  const auto db_accessor = catalog_->GetAccessor(common::ManagedPointer(txn), db_oid, DISABLED);
  NOISEPAGE_ASSERT(db_accessor != nullptr, "Catalog failed to provide a CatalogAccessor. Was the db_oid still valid?");

  const auto result = db_accessor->DropNamespace(ns_oid);
  if (result) {
    txn_manager_->Commit(txn, transaction::TransactionUtil::EmptyCallback, nullptr);
  } else {
    txn_manager_->Abort(txn);
  }
  return result;
}

void TrafficCop::UpdateQueryCacheTimestamp() { query_cache_timestamp_ = txn_manager_->GetCurrentTimestamp(); }

}  // namespace noisepage::trafficcop
