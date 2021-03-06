#pragma once

#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "catalog/catalog_accessor.h"
#include "catalog/catalog_defs.h"

namespace noisepage {

namespace parser {
struct ColumnDefinition;
class ColumnValueExpression;
class CreateStatement;
class SelectStatement;
class TableRef;
class TableStarExpression;
}  // namespace parser

namespace transaction {
class TransactionContext;
}

namespace catalog {
class Catalog;
class Schema;
}  // namespace catalog

namespace binder {

/**
 * @brief Store the visible table aliases and the corresponding <db_id, table_id>
 * tuple. Also record the upper level context when traversing into the nested
 * query. This context keeps track of all the table aliases to which columns
 * in the current level can bind.
 */
class BinderContext {
 public:
  /** TableMetadata is currently a tuple of database oid, table oid, and schema of the table. */
  using TableMetadata = std::tuple<catalog::db_oid_t, catalog::table_oid_t, catalog::Schema>;

  /**
   * Initializes the BinderContext object which has an empty regular table map and an empty nested table map.
   * It also takes in a pointer to the binder context's upper context, and the constructor determines the depth of the
   * current context based on the upper context. These two fields are used in nested queries.
   * @param upper_context Pointer to the upper level binder context of the current binder context.
   */
  explicit BinderContext(common::ManagedPointer<BinderContext> upper_context) : upper_context_(upper_context) {
    if (upper_context != nullptr) depth_ = upper_context->depth_ + 1;
  }

  /**
   * Update the table alias map given a table reference (in the from clause)
   * @param accessor Pointer to the catalog accessor object
   * @param table_ref Pointer to the table ref object
   * @param db_id oid of the database
   */
  void AddRegularTable(common::ManagedPointer<catalog::CatalogAccessor> accessor,
                       common::ManagedPointer<parser::TableRef> table_ref, catalog::db_oid_t db_id);

  /**
   * Update the table alias map given a table reference (in the from clause)
   * @param accessor Pointer to the catalog accessor object
   * @param db_id oid of the database
   * @param namespace_name Name of the namespace
   * @param table_name Name of the table
   * @param table_alias Alias of the table
   */
  void AddRegularTable(common::ManagedPointer<catalog::CatalogAccessor> accessor, catalog::db_oid_t db_id,
                       const std::string &namespace_name, const std::string &table_name,
                       const std::string &table_alias);

  /**
   * Update the nested table alias map
   * @param table_alias Alias of the table
   * @param select_list List of select columns
   * @param col_aliases Aliases to assign to each column in select_list for the temp nested table schema
   */
  void AddNestedTable(const std::string &table_alias,
                      const std::vector<common::ManagedPointer<parser::AbstractExpression>> &select_list,
                      const std::vector<parser::AliasType> &col_aliases);

  /**
   * Adds a Common Table Expression table to the binder. Currently, this adds it to the nested table aliases map.
   * @param table_name Name of the cte table
   * @param select_list List of selected columns that form the query to build the CTE
   * @param col_aliases Aliases for each column (this must be of the same size as the select_list vector)
   */
  void AddCTETable(const std::string &table_name,
                   const std::vector<common::ManagedPointer<parser::AbstractExpression>> &select_list,
                   const std::vector<parser::AliasType> &col_aliases);

  /**
   * Update the nested table alias map to create a copy of CTE table's entry for given alias.
   * @param cte_table_name CTE table name
   * @param table_alias Alias of the table
   */
  void AddCTETableAlias(const std::string &cte_table_name, const std::string &table_alias);

  /**
   * Add the new table by update the nested table alias map. This is called only in create table statement.
   * We insert the new table information to the nested table alias map because the structure of the attribute matches
   * the information we have about the new table; the name of the attribute might confuse people.
   * @param new_table_name Name of the new table
   * @param new_columns List of column definations of the new table
   */
  void AddNewTable(const std::string &new_table_name,
                   const std::vector<common::ManagedPointer<parser::ColumnDefinition>> &new_columns);

  /**
   * Check if the current context has any table
   */
  bool HasTables() { return (!regular_table_alias_map_.empty() || !nested_table_alias_map_.empty()); }

  /**
   * Check if the column name is in the schema
   * @param schema Schema object
   * @param col_name Name of the column
   * @return true if the column is in the schema, false otherwise
   */
  static bool ColumnInSchema(const catalog::Schema &schema, const std::string &col_name);

  /**
   * Construct the column position tuple given column name and the corresponding column value expression.
   * Note that this is just a helper function and it is independent of the context.
   * @param col_name Name of the column
   * @param tuple Tuple of database oid, table oid, and schema object
   * @param expr Column value expression
   */
  static void SetColumnPosTuple(const std::string &col_name,
                                std::tuple<catalog::db_oid_t, catalog::table_oid_t, catalog::Schema> tuple,
                                common::ManagedPointer<parser::ColumnValueExpression> expr);

  /**
   * Set the table_name for a column value expression to the name used in the select statement
   * @param expr Column value expression to modify
   * @param node Select statement
   */
  void SetTableName(common::ManagedPointer<parser::ColumnValueExpression> expr,
                    common::ManagedPointer<parser::SelectStatement> node);

  /**
   * Construct the column position tuple given only the column value expression and the context.
   * Also internally update the column value expression according to the values in the context
   * @param expr Column value expression
   * @return Returns true if the column is found in the alias maps of the current context; false otherwise
   */
  bool SetColumnPosTuple(common::ManagedPointer<parser::ColumnValueExpression> expr);

  /**
   * Check if the table alias can be found in the alias maps of the current context or the upper contexts.
   * This function internally updates the depth of the expression if the alias is successfully found
   * @param alias Table alias
   * @param expr Column value expression
   * @param tuple Tuple of database oid, table oid, and schema object
   * @return Return true if the alias is found, false otherwise
   */
  bool GetRegularTableObj(
      const std::string &alias, common::ManagedPointer<parser::ColumnValueExpression> expr,
      common::ManagedPointer<std::tuple<catalog::db_oid_t, catalog::table_oid_t, catalog::Schema>> tuple);

  /**
   * Check if the table, represented by the table alias, has the column indicated by the column name.
   * This function internally updates the information of the expression if the column is successfully found
   * @param alias Table alias
   * @param col_name Name of the column
   * @param expr Column value expression
   * @return Return true if the column is found, false otherwise
   */
  bool CheckNestedTableColumn(const parser::AliasType &alias, const std::string &col_name,
                              common::ManagedPointer<parser::ColumnValueExpression> expr);

  /**
   * Get the pointer to the upper context of the current context
   * @return Pointer to the upper binder context
   */
  common::ManagedPointer<BinderContext> GetUpperContext() { return upper_context_; }

  /**
   * Set the upper context of the current context.
   * @param upper_context Pointer to the upper binder context
   */
  void SetUpperContext(common::ManagedPointer<BinderContext> upper_context) { upper_context_ = upper_context; }

  /**
   * Set the depth of the current context
   * @param depth Depth of the context
   */
  void SetDepth(int depth) { depth_ = depth; }

  /**
   * Get the depth of the current context
   * @return depth of the current binder context
   */
  int GetDepth() { return depth_; }

  /**
   * Generate list of column value expression that covers all columns in the alias maps of the current context
   * @param table_star Describes which table's column value expressions to generate
   * @param parse_result Result generated by the parser. A collection of statements and expressions in the query.
   * @param exprs Pointer to the list of column value expression.
   * The generated column value expressions will be placed in this list.
   */
  void GenerateAllColumnExpressions(
      common::ManagedPointer<parser::TableStarExpression> table_star,
      common::ManagedPointer<parser::ParseResult> parse_result,
      common::ManagedPointer<std::vector<common::ManagedPointer<parser::AbstractExpression>>> exprs);

  /**
   * Return the binder context's metadata for the provided @p table_name.
   * @param table_name the name of the table to look up
   * @return pointer to the {database oid, table oid, schema} corresponding to @p table_name, nullptr if not found
   */
  common::ManagedPointer<TableMetadata> GetTableMapping(const std::string &table_name);

  /**
   * Save mapping from alias name to AliasType in this context
   * @param alias_name alias name to save
   * @param alias_type AliasType to save
   */
  void AddTableAliasMapping(const std::string &alias_name, const parser::AliasType &alias_type);

  /**
   * Check if alias is saved in this context
   * @param alias_name alias name to check for
   * @return true if alias_name is saved in this context, false otherwise
   */
  bool HasTableAlias(const std::string &alias_name);

  /**
   * Retrieves the AliasType saved in this context corresponding to alias name
   * @pre Unless you are sure the alias exists at this level you should call HasTableAlias first
   * @param alias_name name of alias to retrieve
   * @return AliasType with name alias_name
   */
  parser::AliasType &GetTableAlias(const std::string &alias_name);

  /**
   * Retrieve the alias saved in this context corresponding to alias_name, if none is found then create a new alias
   * using alias_name
   * @param alias_name name of the alias we are looking for
   * @return AliasType corresponding to alias_name
   */
  parser::AliasType GetOrCreateTableAlias(const std::string &alias_name);

  /**
   * Starting at the current context, traverse up to higher level contexts until we find an AliasType corresponding to
   * alias_name. If we don't find one, then create a new AliasType using alias_name.
   * @param alias_name name of alias to look for
   * @return AliasType corresponding to alias_name
   */
  parser::AliasType FindTableAlias(const std::string &alias_name);

 private:
  /**
   * Map table alias to its metadata
   */
  std::unordered_map<std::string, TableMetadata> regular_table_alias_map_;

  /**
   * Tracks the order in which table alias's were entered
   */
  std::vector<std::string> regular_table_alias_list_;

  /**
   * Map the table alias to maps which is from table alias to the value type
   */
  std::unordered_map<std::string, std::unordered_map<parser::AliasType, execution::sql::SqlTypeId>>
      nested_table_alias_map_;

  /**
   * Map the table alias name to table AliasType
   */
  std::unordered_map<std::string, parser::AliasType> table_alias_name_to_type_map_;

  /**
   * Upper binder context of the current binder context
   */
  common::ManagedPointer<BinderContext> upper_context_;

  /**
   * depth of the current binder context
   */
  int depth_ = 0;
};

}  // namespace binder
}  // namespace noisepage
