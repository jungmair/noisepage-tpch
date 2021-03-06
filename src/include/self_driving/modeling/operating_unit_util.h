#pragma once

#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "parser/expression/abstract_expression.h"
#include "self_driving/modeling/operating_unit.h"
#include "self_driving/modeling/operating_unit_defs.h"

namespace noisepage::selfdriving {

/**
 * Utility class for OperatingUnits
 * Includes some conversion/utility code
 */
class OperatingUnitUtil {
 public:
  /**
   * Derive the type of computation
   * @param expr Expression
   * @return type of computation
   */
  static execution::sql::SqlTypeId DeriveComputation(common::ManagedPointer<parser::AbstractExpression> expr) {
    if (expr->GetChildrenSize() == 0) {
      // Not a computation
      return execution::sql::SqlTypeId::Invalid;
    }

    auto lchild = expr->GetChild(0);
    if (lchild->GetReturnValueType() != execution::sql::SqlTypeId::Invalid) {
      return lchild->GetReturnValueType();
    }

    if (expr->GetChildrenSize() > 1) {
      auto rchild = expr->GetChild(1);
      if (rchild->GetReturnValueType() != execution::sql::SqlTypeId::Invalid) {
        return rchild->GetReturnValueType();
      }
    }

    return execution::sql::SqlTypeId::Invalid;
  }

  /**
   * Converts a expression to selfdriving::ExecutionOperatingUnitType
   *
   * Function returns selfdriving::ExecutionOperatingUnitType::INVALID if the
   * parser::ExpressionType does not have an equivalent conversion.
   *
   * @param expr Expression
   * @return converted equivalent selfdriving::ExecutionOperatingUnitType
   */
  static std::pair<execution::sql::SqlTypeId, ExecutionOperatingUnitType> ConvertExpressionType(
      common::ManagedPointer<parser::AbstractExpression> expr) {
    auto type = DeriveComputation(expr);
    switch (expr->GetExpressionType()) {
      case parser::ExpressionType::AGGREGATE_COUNT:
        return std::make_pair(type, ExecutionOperatingUnitType::OP_INTEGER_PLUS_OR_MINUS);
      case parser::ExpressionType::AGGREGATE_SUM:
      case parser::ExpressionType::AGGREGATE_AVG:
      case parser::ExpressionType::OPERATOR_PLUS:
      case parser::ExpressionType::OPERATOR_MINUS: {
        switch (type) {
          case execution::sql::SqlTypeId::TinyInt:
          case execution::sql::SqlTypeId::SmallInt:
          case execution::sql::SqlTypeId::Integer:
          case execution::sql::SqlTypeId::BigInt:
            return std::make_pair(type, ExecutionOperatingUnitType::OP_INTEGER_PLUS_OR_MINUS);
          case execution::sql::SqlTypeId::Double:
            return std::make_pair(type, ExecutionOperatingUnitType::OP_REAL_PLUS_OR_MINUS);
          default:
            return std::make_pair(type, ExecutionOperatingUnitType::INVALID);
        }
      }
      case parser::ExpressionType::OPERATOR_MULTIPLY: {
        switch (type) {
          case execution::sql::SqlTypeId::TinyInt:
          case execution::sql::SqlTypeId::SmallInt:
          case execution::sql::SqlTypeId::Integer:
          case execution::sql::SqlTypeId::BigInt:
            return std::make_pair(type, ExecutionOperatingUnitType::OP_INTEGER_MULTIPLY);
          case execution::sql::SqlTypeId::Double:
            return std::make_pair(type, ExecutionOperatingUnitType::OP_REAL_MULTIPLY);
          default:
            return std::make_pair(type, ExecutionOperatingUnitType::INVALID);
        }
      }
      case parser::ExpressionType::OPERATOR_DIVIDE: {
        switch (type) {
          case execution::sql::SqlTypeId::TinyInt:
          case execution::sql::SqlTypeId::SmallInt:
          case execution::sql::SqlTypeId::Integer:
          case execution::sql::SqlTypeId::BigInt:
            return std::make_pair(type, ExecutionOperatingUnitType::OP_INTEGER_DIVIDE);
          case execution::sql::SqlTypeId::Double:
            return std::make_pair(type, ExecutionOperatingUnitType::OP_REAL_DIVIDE);
          default:
            return std::make_pair(type, ExecutionOperatingUnitType::INVALID);
        }
      }
      case parser::ExpressionType::AGGREGATE_MAX:
      case parser::ExpressionType::AGGREGATE_MIN:
      case parser::ExpressionType::COMPARE_EQUAL:
      case parser::ExpressionType::COMPARE_NOT_EQUAL:
      case parser::ExpressionType::COMPARE_LESS_THAN:
      case parser::ExpressionType::COMPARE_GREATER_THAN:
      case parser::ExpressionType::COMPARE_LESS_THAN_OR_EQUAL_TO:
      case parser::ExpressionType::COMPARE_GREATER_THAN_OR_EQUAL_TO: {
        switch (type) {
          case execution::sql::SqlTypeId::Boolean:
            return std::make_pair(type, ExecutionOperatingUnitType::OP_BOOL_COMPARE);
          case execution::sql::SqlTypeId::TinyInt:
          case execution::sql::SqlTypeId::SmallInt:
          case execution::sql::SqlTypeId::Integer:
          case execution::sql::SqlTypeId::BigInt:
            return std::make_pair(type, ExecutionOperatingUnitType::OP_INTEGER_COMPARE);
          case execution::sql::SqlTypeId::Double:
            return std::make_pair(type, ExecutionOperatingUnitType::OP_REAL_COMPARE);
          case execution::sql::SqlTypeId::Timestamp:
          case execution::sql::SqlTypeId::Date:
            return std::make_pair(type, ExecutionOperatingUnitType::OP_INTEGER_COMPARE);
          case execution::sql::SqlTypeId::Varchar:
          case execution::sql::SqlTypeId::Varbinary:
            return std::make_pair(type, ExecutionOperatingUnitType::OP_VARCHAR_COMPARE);
          default:
            return std::make_pair(type, ExecutionOperatingUnitType::INVALID);
        }
      }
      default:
        return std::make_pair(type, ExecutionOperatingUnitType::INVALID);
    }
  }

  /**
   * Extracts features from an expression into a vector
   * @param expr Expression to extract features from
   * @return vector of extracted features
   */
  static std::vector<std::pair<execution::sql::SqlTypeId, ExecutionOperatingUnitType>> ExtractFeaturesFromExpression(
      common::ManagedPointer<parser::AbstractExpression> expr) {
    if (expr == nullptr) return std::vector<std::pair<execution::sql::SqlTypeId, ExecutionOperatingUnitType>>();

    std::vector<std::pair<execution::sql::SqlTypeId, ExecutionOperatingUnitType>> feature_types;
    std::queue<common::ManagedPointer<parser::AbstractExpression>> work;
    work.push(expr);

    while (!work.empty()) {
      auto head = work.front();
      work.pop();

      auto feature = ConvertExpressionType(head);
      if (feature.second != ExecutionOperatingUnitType::INVALID) {
        feature_types.push_back(feature);
      }

      for (auto child : head->GetChildren()) {
        work.push(child);
      }
    }

    return feature_types;
  }

  /**
   * Whether or not an operating unit type can be merged
   * @param feature OperatingUnitType to consider
   * @return mergeable or not
   */
  static bool IsOperatingUnitTypeMergeable(ExecutionOperatingUnitType feature) {
    return feature > ExecutionOperatingUnitType::PLAN_OPS_DELIMITER;
  }

  /**
   * Determines whether the operating unit type is a blocking OU
   * @param feature OperatingUnitType to consider
   * @return blocking or not
   */
  static bool IsOperatingUnitTypeBlocking(ExecutionOperatingUnitType feature);

  /**
   * Gets the non-parallel type for the OU feature
   * @param feature Parallel OU
   * @return Corresponding non-parallel OU or INVALID
   */
  static ExecutionOperatingUnitType GetNonParallelType(ExecutionOperatingUnitType feature);

  /** @return The ExecutionOperatingUnitFeature that has the corresponding type. It must be unique in the vector. */
  static const ExecutionOperatingUnitFeature &GetFeature(execution::translator_id_t translator_id,
                                                         const std::vector<ExecutionOperatingUnitFeature> &features,
                                                         ExecutionOperatingUnitType type) {
    UNUSED_ATTRIBUTE bool found = false;
    size_t idx = 0;
    for (size_t i = 0; i < features.size(); ++i) {
      bool same_translator = translator_id == features[i].GetTranslatorId();
      bool same_feature = type == features[i].GetExecutionOperatingUnitType();
      if (same_translator && same_feature) {
        NOISEPAGE_ASSERT(!found, "There are multiple features of the same type.");
        found = true;
        idx = i;
      }
    }
    NOISEPAGE_ASSERT(found, "The feature was not found.");
    return features[idx];
  }

  /**
   * Converts an ExecutionOperatingUnitType enum to string representation
   * @param f ExecutionOperatingUnitType to convert
   */
  static std::string ExecutionOperatingUnitTypeToString(ExecutionOperatingUnitType f);
};

}  // namespace noisepage::selfdriving
