#include "parser/expression/aggregate_expression.h"

#include "binder/sql_node_visitor.h"
#include "common/hash_util.h"
#include "common/json.h"
#include "spdlog/fmt/fmt.h"

namespace noisepage::parser {

std::unique_ptr<AbstractExpression> AggregateExpression::Copy() const {
  std::vector<std::unique_ptr<AbstractExpression>> children;
  for (const auto &child : GetChildren()) {
    children.emplace_back(child->Copy());
  }
  return CopyWithChildren(std::move(children));
}

std::unique_ptr<AbstractExpression> AggregateExpression::CopyWithChildren(
    std::vector<std::unique_ptr<AbstractExpression>> &&children) const {
  auto expr = std::make_unique<AggregateExpression>(GetExpressionType(), std::move(children), IsDistinct());
  expr->SetMutableStateForCopy(*this);
  return expr;
}

void AggregateExpression::DeriveReturnValueType() {
  auto expr_type = this->GetExpressionType();
  switch (expr_type) {
    case ExpressionType::AGGREGATE_COUNT:
      this->SetReturnValueType(execution::sql::SqlTypeId::Integer);
      break;
    // keep the type of the base
    case ExpressionType::AGGREGATE_MAX:
    case ExpressionType::AGGREGATE_MIN:
    case ExpressionType::AGGREGATE_SUM:
      NOISEPAGE_ASSERT(this->GetChildrenSize() >= 1, "No column name given.");
      const_cast<parser::AbstractExpression *>(this->GetChild(0).Get())->DeriveReturnValueType();
      this->SetReturnValueType(this->GetChild(0)->GetReturnValueType());
      break;
    case ExpressionType::AGGREGATE_AVG:
      this->SetReturnValueType(execution::sql::SqlTypeId::Double);
      break;
    case ExpressionType::AGGREGATE_TOP_K:
    case ExpressionType::AGGREGATE_HISTOGRAM:
      this->SetReturnValueType(execution::sql::SqlTypeId::Varbinary);
      break;
    default:
      throw PARSER_EXCEPTION(fmt::format("Not a valid aggregation expression type: %d", static_cast<int>(expr_type)));
  }
}

nlohmann::json AggregateExpression::ToJson() const {
  nlohmann::json j = AbstractExpression::ToJson();
  j["distinct"] = distinct_;
  return j;
}

std::vector<std::unique_ptr<AbstractExpression>> AggregateExpression::FromJson(const nlohmann::json &j) {
  std::vector<std::unique_ptr<AbstractExpression>> exprs;
  auto e1 = AbstractExpression::FromJson(j);
  exprs.insert(exprs.end(), std::make_move_iterator(e1.begin()), std::make_move_iterator(e1.end()));
  distinct_ = j.at("distinct").get<bool>();
  return exprs;
}

void AggregateExpression::Accept(common::ManagedPointer<binder::SqlNodeVisitor> v) {
  v->Visit(common::ManagedPointer(this));
}

common::hash_t AggregateExpression::Hash() const {
  common::hash_t hash = AbstractExpression::Hash();
  hash = common::HashUtil::CombineHashes(hash, common::HashUtil::Hash(distinct_));
  return hash;
}
bool AggregateExpression::RequiresCleanup() const {
  auto expr_type = this->GetExpressionType();
  switch (expr_type) {
    case ExpressionType::AGGREGATE_COUNT:
    case ExpressionType::AGGREGATE_MAX:
    case ExpressionType::AGGREGATE_MIN:
    case ExpressionType::AGGREGATE_SUM:
    case ExpressionType::AGGREGATE_AVG:
      return false;
    case ExpressionType::AGGREGATE_TOP_K:
    case ExpressionType::AGGREGATE_HISTOGRAM:
      return true;
    default:
      throw PARSER_EXCEPTION(fmt::format("Not a valid aggregation expression type: %d", static_cast<int>(expr_type)));
  }
}

DEFINE_JSON_BODY_DECLARATIONS(AggregateExpression);

}  // namespace noisepage::parser
