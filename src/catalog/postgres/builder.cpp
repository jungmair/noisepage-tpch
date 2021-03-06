#include "catalog/postgres/builder.h"

#include <utility>
#include <vector>

#include "catalog/database_catalog.h"
#include "catalog/index_schema.h"
#include "catalog/postgres/pg_attribute.h"
#include "catalog/postgres/pg_class.h"
#include "catalog/postgres/pg_constraint.h"
#include "catalog/postgres/pg_database.h"
#include "catalog/postgres/pg_index.h"
#include "catalog/postgres/pg_language.h"
#include "catalog/postgres/pg_namespace.h"
#include "catalog/postgres/pg_proc.h"
#include "catalog/postgres/pg_statistic.h"
#include "catalog/postgres/pg_type.h"
#include "catalog/schema.h"
#include "parser/expression/abstract_expression.h"
#include "parser/expression/column_value_expression.h"
#include "parser/expression/constant_value_expression.h"
#include "storage/index/index_builder.h"
#include "storage/sql_table.h"

namespace noisepage::catalog::postgres {

constexpr uint8_t MAX_NAME_LENGTH = 63;  // This mimics PostgreSQL behavior

Schema Builder::GetDatabaseTableSchema() {
  std::vector<Schema::Column> columns;

  columns.emplace_back("datoid", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgDatabase::DATOID.oid_);

  columns.emplace_back("datname", execution::sql::SqlTypeId::Varchar, MAX_NAME_LENGTH, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varchar));
  columns.back().SetOid(PgDatabase::DATNAME.oid_);

  columns.emplace_back("pointer", execution::sql::SqlTypeId::BigInt, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::BigInt));
  columns.back().SetOid(PgDatabase::DAT_CATALOG.oid_);

  return Schema(columns);
}

IndexSchema Builder::GetDatabaseOidIndexSchema() {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back(
      "datoid", execution::sql::SqlTypeId::Integer, false,
      parser::ColumnValueExpression(INVALID_DATABASE_OID, PgDatabase::DATABASE_TABLE_OID, PgDatabase::DATOID.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  // Primary
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, true, true, false, true, options);

  return schema;
}

IndexSchema Builder::GetDatabaseNameIndexSchema() {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back(
      "datname", execution::sql::SqlTypeId::Varchar, MAX_NAME_LENGTH, false,
      parser::ColumnValueExpression(INVALID_DATABASE_OID, PgDatabase::DATABASE_TABLE_OID, PgDatabase::DATNAME.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  // Unique, not primary
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, true, false, false, true, options);

  return schema;
}

DatabaseCatalog *Builder::CreateDatabaseCatalog(
    const common::ManagedPointer<storage::BlockStore> block_store, const db_oid_t oid,
    const common::ManagedPointer<storage::GarbageCollector> garbage_collector) {
  auto dbc = new DatabaseCatalog(oid, garbage_collector);

  dbc->pg_core_.namespaces_ = new storage::SqlTable(block_store, Builder::GetNamespaceTableSchema());
  dbc->pg_core_.classes_ = new storage::SqlTable(block_store, Builder::GetClassTableSchema());
  dbc->pg_core_.indexes_ = new storage::SqlTable(block_store, Builder::GetIndexTableSchema());
  dbc->pg_core_.columns_ = new storage::SqlTable(block_store, Builder::GetColumnTableSchema());
  dbc->pg_type_.types_ = new storage::SqlTable(block_store, Builder::GetTypeTableSchema());
  dbc->pg_constraint_.constraints_ = new storage::SqlTable(block_store, Builder::GetConstraintTableSchema());
  dbc->pg_language_.languages_ = new storage::SqlTable(block_store, Builder::GetLanguageTableSchema());
  dbc->pg_proc_.procs_ = new storage::SqlTable(block_store, Builder::GetProcTableSchema());
  dbc->pg_stat_.statistics_ = new storage::SqlTable(block_store, Builder::GetStatisticTableSchema());

  // Indexes on pg_namespace
  dbc->pg_core_.namespaces_oid_index_ =
      Builder::BuildUniqueIndex(Builder::GetNamespaceOidIndexSchema(oid), PgNamespace::NAMESPACE_OID_INDEX_OID);
  dbc->pg_core_.namespaces_name_index_ =
      Builder::BuildUniqueIndex(Builder::GetNamespaceNameIndexSchema(oid), PgNamespace::NAMESPACE_NAME_INDEX_OID);

  // Indexes on pg_class
  dbc->pg_core_.classes_oid_index_ =
      Builder::BuildUniqueIndex(Builder::GetClassOidIndexSchema(oid), PgClass::CLASS_OID_INDEX_OID);
  dbc->pg_core_.classes_name_index_ =
      Builder::BuildUniqueIndex(Builder::GetClassNameIndexSchema(oid), PgClass::CLASS_NAME_INDEX_OID);
  dbc->pg_core_.classes_namespace_index_ =
      Builder::BuildLookupIndex(Builder::GetClassNamespaceIndexSchema(oid), PgClass::CLASS_NAMESPACE_INDEX_OID);

  // Indexes on pg_index
  dbc->pg_core_.indexes_oid_index_ =
      Builder::BuildUniqueIndex(Builder::GetIndexOidIndexSchema(oid), PgIndex::INDEX_OID_INDEX_OID);
  dbc->pg_core_.indexes_table_index_ =
      Builder::BuildLookupIndex(Builder::GetIndexTableIndexSchema(oid), PgIndex::INDEX_TABLE_INDEX_OID);

  // Indexes on pg_attribute
  dbc->pg_core_.columns_oid_index_ =
      Builder::BuildUniqueIndex(Builder::GetColumnOidIndexSchema(oid), PgAttribute::COLUMN_OID_INDEX_OID);
  dbc->pg_core_.columns_name_index_ =
      Builder::BuildUniqueIndex(Builder::GetColumnNameIndexSchema(oid), PgAttribute::COLUMN_NAME_INDEX_OID);

  // Indexes on pg_type
  dbc->pg_type_.types_oid_index_ =
      Builder::BuildUniqueIndex(Builder::GetTypeOidIndexSchema(oid), PgType::TYPE_OID_INDEX_OID);
  dbc->pg_type_.types_name_index_ =
      Builder::BuildUniqueIndex(Builder::GetTypeNameIndexSchema(oid), PgType::TYPE_NAME_INDEX_OID);
  dbc->pg_type_.types_namespace_index_ =
      Builder::BuildLookupIndex(Builder::GetTypeNamespaceIndexSchema(oid), PgType::TYPE_NAMESPACE_INDEX_OID);

  // Indexes on pg_constraint
  dbc->pg_constraint_.constraints_oid_index_ =
      Builder::BuildUniqueIndex(Builder::GetConstraintOidIndexSchema(oid), PgConstraint::CONSTRAINT_OID_INDEX_OID);
  dbc->pg_constraint_.constraints_name_index_ =
      Builder::BuildUniqueIndex(Builder::GetConstraintNameIndexSchema(oid), PgConstraint::CONSTRAINT_NAME_INDEX_OID);
  dbc->pg_constraint_.constraints_namespace_index_ = Builder::BuildLookupIndex(
      Builder::GetConstraintNamespaceIndexSchema(oid), PgConstraint::CONSTRAINT_NAMESPACE_INDEX_OID);
  dbc->pg_constraint_.constraints_table_index_ =
      Builder::BuildLookupIndex(Builder::GetConstraintTableIndexSchema(oid), PgConstraint::CONSTRAINT_TABLE_INDEX_OID);
  dbc->pg_constraint_.constraints_index_index_ =
      Builder::BuildLookupIndex(Builder::GetConstraintIndexIndexSchema(oid), PgConstraint::CONSTRAINT_INDEX_INDEX_OID);
  dbc->pg_constraint_.constraints_foreigntable_index_ = Builder::BuildLookupIndex(
      Builder::GetConstraintForeignTableIndexSchema(oid), PgConstraint::CONSTRAINT_FOREIGNTABLE_INDEX_OID);

  // Indexes on pg_language
  dbc->pg_language_.languages_oid_index_ =
      Builder::BuildUniqueIndex(Builder::GetLanguageOidIndexSchema(oid), PgLanguage::LANGUAGE_OID_INDEX_OID);
  dbc->pg_language_.languages_name_index_ =
      Builder::BuildUniqueIndex(Builder::GetLanguageNameIndexSchema(oid), PgLanguage::LANGUAGE_NAME_INDEX_OID);

  // Indexes on pg_proc
  dbc->pg_proc_.procs_oid_index_ =
      Builder::BuildUniqueIndex(Builder::GetProcOidIndexSchema(oid), PgProc::PRO_OID_INDEX_OID);
  dbc->pg_proc_.procs_name_index_ =
      Builder::BuildLookupIndex(Builder::GetProcNameIndexSchema(oid), PgProc::PRO_NAME_INDEX_OID);

  // Indexes on pg_statistic
  dbc->pg_stat_.statistic_oid_index_ =
      Builder::BuildUniqueIndex(Builder::GetStatisticOidIndexSchema(oid), PgStatistic::STATISTIC_OID_INDEX_OID);

  dbc->next_oid_.store(START_OID);

  return dbc;
}

Schema Builder::GetColumnTableSchema() {
  std::vector<Schema::Column> columns;

  columns.emplace_back("attnum", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgAttribute::ATTNUM.oid_);

  columns.emplace_back("attrelid", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgAttribute::ATTRELID.oid_);

  columns.emplace_back("attname", execution::sql::SqlTypeId::Varchar, MAX_NAME_LENGTH, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varchar));
  columns.back().SetOid(PgAttribute::ATTNAME.oid_);

  columns.emplace_back("atttypid", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgAttribute::ATTTYPID.oid_);

  columns.emplace_back("attlen", execution::sql::SqlTypeId::SmallInt, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::SmallInt));
  columns.back().SetOid(PgAttribute::ATTLEN.oid_);

  columns.emplace_back("atttypmod", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgAttribute::ATTTYPMOD.oid_);

  columns.emplace_back("attnotnull", execution::sql::SqlTypeId::Boolean, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Boolean));
  columns.back().SetOid(PgAttribute::ATTNOTNULL.oid_);

  columns.emplace_back("adsrc", execution::sql::SqlTypeId::Varchar, 4096, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varchar));
  columns.back().SetOid(PgAttribute::ADSRC.oid_);

  return Schema(columns);
}

Schema Builder::GetClassTableSchema() {
  std::vector<Schema::Column> columns;

  columns.emplace_back("reloid", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgClass::RELOID.oid_);

  columns.emplace_back("relname", execution::sql::SqlTypeId::Varchar, MAX_NAME_LENGTH, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varchar));
  columns.back().SetOid(PgClass::RELNAME.oid_);

  columns.emplace_back("relnamespace", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgClass::RELNAMESPACE.oid_);

  columns.emplace_back("relkind", execution::sql::SqlTypeId::TinyInt, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::TinyInt));
  columns.back().SetOid(PgClass::RELKIND.oid_);

  // TODO(wz2): Technically this should be a text[] from https://www.postgresql.org/docs/8.3/catalog-pg-class.html.
  // However, we currently do not support array types. For now, the options supplied to CREATE INDEX are dumped
  // in JSON form and stored in this column.
  columns.emplace_back("reloptions", execution::sql::SqlTypeId::Varchar, MAX_NAME_LENGTH, true,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varchar));
  columns.back().SetOid(PgClass::RELOPTIONS.oid_);

  columns.emplace_back("schema", execution::sql::SqlTypeId::BigInt, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::BigInt));
  columns.back().SetOid(PgClass::REL_SCHEMA.oid_);

  columns.emplace_back("pointer", execution::sql::SqlTypeId::BigInt, true,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::BigInt));
  columns.back().SetOid(PgClass::REL_PTR.oid_);

  columns.emplace_back("nextcoloid", execution::sql::SqlTypeId::Integer, true,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgClass::REL_NEXTCOLOID.oid_);

  return Schema(columns);
}

Schema Builder::GetConstraintTableSchema() {
  std::vector<Schema::Column> columns;

  columns.emplace_back("conoid", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgConstraint::CONOID.oid_);

  columns.emplace_back("conname", execution::sql::SqlTypeId::Varchar, MAX_NAME_LENGTH, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varchar));
  columns.back().SetOid(PgConstraint::CONNAME.oid_);

  columns.emplace_back("connamespace", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgConstraint::CONNAMESPACE.oid_);

  columns.emplace_back("contype", execution::sql::SqlTypeId::TinyInt, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::TinyInt));
  columns.back().SetOid(PgConstraint::CONTYPE.oid_);

  columns.emplace_back("condeferrable", execution::sql::SqlTypeId::Boolean, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Boolean));
  columns.back().SetOid(PgConstraint::CONDEFERRABLE.oid_);

  columns.emplace_back("condeferred", execution::sql::SqlTypeId::Boolean, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Boolean));
  columns.back().SetOid(PgConstraint::CONDEFERRED.oid_);

  columns.emplace_back("convalidated", execution::sql::SqlTypeId::Boolean, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Boolean));
  columns.back().SetOid(PgConstraint::CONVALIDATED.oid_);

  columns.emplace_back("conrelid", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgConstraint::CONRELID.oid_);

  columns.emplace_back("conindid", execution::sql::SqlTypeId::Integer, true,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgConstraint::CONINDID.oid_);

  columns.emplace_back("confrelid", execution::sql::SqlTypeId::Integer, true,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgConstraint::CONFRELID.oid_);

  columns.emplace_back("conbin", execution::sql::SqlTypeId::BigInt, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::BigInt));
  columns.back().SetOid(PgConstraint::CONBIN.oid_);

  columns.emplace_back("consrc", execution::sql::SqlTypeId::Varchar, 4096, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varchar));
  columns.back().SetOid(PgConstraint::CONSRC.oid_);

  return Schema(columns);
}

Schema Builder::GetIndexTableSchema() {
  std::vector<Schema::Column> columns;

  columns.emplace_back("indoid", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgIndex::INDOID.oid_);

  columns.emplace_back("indrelid", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgIndex::INDRELID.oid_);

  columns.emplace_back("indisunique", execution::sql::SqlTypeId::Boolean, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Boolean));
  columns.back().SetOid(PgIndex::INDISUNIQUE.oid_);

  columns.emplace_back("indisprimary", execution::sql::SqlTypeId::Boolean, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Boolean));
  columns.back().SetOid(PgIndex::INDISPRIMARY.oid_);

  columns.emplace_back("indisexclusion", execution::sql::SqlTypeId::Boolean, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Boolean));
  columns.back().SetOid(PgIndex::INDISEXCLUSION.oid_);

  columns.emplace_back("indimmediate", execution::sql::SqlTypeId::Boolean, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Boolean));
  columns.back().SetOid(PgIndex::INDIMMEDIATE.oid_);

  columns.emplace_back("indisvalid", execution::sql::SqlTypeId::Boolean, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Boolean));
  columns.back().SetOid(PgIndex::INDISVALID.oid_);

  columns.emplace_back("indisready", execution::sql::SqlTypeId::Boolean, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Boolean));
  columns.back().SetOid(PgIndex::INDISREADY.oid_);

  columns.emplace_back("indislive", execution::sql::SqlTypeId::Boolean, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Boolean));
  columns.back().SetOid(PgIndex::INDISLIVE.oid_);

  columns.emplace_back("implementation", execution::sql::SqlTypeId::TinyInt, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::TinyInt));
  columns.back().SetOid(PgIndex::IND_TYPE.oid_);

  return Schema(columns);
}

Schema Builder::GetNamespaceTableSchema() {
  std::vector<Schema::Column> columns;

  columns.emplace_back("nspoid", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgNamespace::NSPOID.oid_);

  columns.emplace_back("nspname", execution::sql::SqlTypeId::Varchar, MAX_NAME_LENGTH, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varchar));
  columns.back().SetOid(PgNamespace::NSPNAME.oid_);

  return Schema(columns);
}

Schema Builder::GetTypeTableSchema() {
  std::vector<Schema::Column> columns;

  columns.emplace_back("typoid", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgType::TYPOID.oid_);

  columns.emplace_back("typname", execution::sql::SqlTypeId::Varchar, MAX_NAME_LENGTH, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varchar));
  columns.back().SetOid(PgType::TYPNAME.oid_);

  columns.emplace_back("typnamespace", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgType::TYPNAMESPACE.oid_);

  columns.emplace_back("typlen", execution::sql::SqlTypeId::SmallInt, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::SmallInt));
  columns.back().SetOid(PgType::TYPLEN.oid_);

  columns.emplace_back("typbyval", execution::sql::SqlTypeId::Boolean, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Boolean));
  columns.back().SetOid(PgType::TYPBYVAL.oid_);

  columns.emplace_back("typtype", execution::sql::SqlTypeId::TinyInt, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::TinyInt));
  columns.back().SetOid(PgType::TYPTYPE.oid_);

  return Schema(columns);
}

Schema Builder::GetLanguageTableSchema() {
  std::vector<Schema::Column> columns;

  columns.emplace_back("lanoid", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgLanguage::LANOID.oid_);

  columns.emplace_back("lanname", execution::sql::SqlTypeId::Varchar, MAX_NAME_LENGTH, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varchar));
  columns.back().SetOid(PgLanguage::LANNAME.oid_);

  columns.emplace_back("lanispl", execution::sql::SqlTypeId::Boolean, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Boolean));
  columns.back().SetOid(PgLanguage::LANISPL.oid_);

  columns.emplace_back("lanpltrusted", execution::sql::SqlTypeId::Boolean, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Boolean));
  columns.back().SetOid(PgLanguage::LANPLTRUSTED.oid_);

  columns.emplace_back("lanplcallfoid", execution::sql::SqlTypeId::Integer, true,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgLanguage::LANPLCALLFOID.oid_);

  columns.emplace_back("laninline", execution::sql::SqlTypeId::Integer, true,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgLanguage::LANINLINE.oid_);

  columns.emplace_back("lanvalidator", execution::sql::SqlTypeId::Integer, true,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgLanguage::LANVALIDATOR.oid_);

  return Schema(columns);
}

IndexSchema Builder::GetNamespaceOidIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back("nspoid", execution::sql::SqlTypeId::Integer, false,
                       parser::ColumnValueExpression(db, PgNamespace::NAMESPACE_TABLE_OID, PgNamespace::NSPOID.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  // Primary
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, true, true, false, true, options);

  return schema;
}

IndexSchema Builder::GetNamespaceNameIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back("nspname", execution::sql::SqlTypeId::Varchar, MAX_NAME_LENGTH, false,
                       parser::ColumnValueExpression(db, PgNamespace::NAMESPACE_TABLE_OID, PgNamespace::NSPNAME.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  // Unique, not primary
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, true, false, false, true, options);

  return schema;
}

IndexSchema Builder::GetClassOidIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back("reloid", execution::sql::SqlTypeId::Integer, false,
                       parser::ColumnValueExpression(db, PgClass::CLASS_TABLE_OID, PgClass::RELOID.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  // Primary
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, true, true, false, true, options);

  return schema;
}

IndexSchema Builder::GetClassNameIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back("relnamespace", execution::sql::SqlTypeId::Integer, false,
                       parser::ColumnValueExpression(db, PgClass::CLASS_TABLE_OID, PgClass::RELNAMESPACE.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  columns.emplace_back("relname", execution::sql::SqlTypeId::Varchar, MAX_NAME_LENGTH, false,
                       parser::ColumnValueExpression(db, PgClass::CLASS_TABLE_OID, PgClass::RELNAME.oid_));
  columns.back().SetOid(indexkeycol_oid_t(2));

  // Unique, not primary
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, true, false, false, true, options);

  return schema;
}

IndexSchema Builder::GetClassNamespaceIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back("relnamespace", execution::sql::SqlTypeId::Integer, false,
                       parser::ColumnValueExpression(db, PgClass::CLASS_TABLE_OID, PgClass::RELNAMESPACE.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  // Not unique
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, false, false, false, true, options);

  return schema;
}

IndexSchema Builder::GetIndexOidIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back("indoid", execution::sql::SqlTypeId::Integer, false,
                       parser::ColumnValueExpression(db, PgIndex::INDEX_TABLE_OID, PgIndex::INDOID.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  // Primary
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, true, true, false, true, options);

  return schema;
}

IndexSchema Builder::GetIndexTableIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back("indrelid", execution::sql::SqlTypeId::Integer, false,
                       parser::ColumnValueExpression(db, PgIndex::INDEX_TABLE_OID, PgIndex::INDRELID.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  // Not unique
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, false, false, false, true, options);

  return schema;
}

IndexSchema Builder::GetColumnOidIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back("attrelid", execution::sql::SqlTypeId::Integer, false,
                       parser::ColumnValueExpression(db, PgAttribute::COLUMN_TABLE_OID, PgAttribute::ATTRELID.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  columns.emplace_back("attnum", execution::sql::SqlTypeId::Integer, false,
                       parser::ColumnValueExpression(db, PgAttribute::COLUMN_TABLE_OID, PgAttribute::ATTNUM.oid_));
  columns.back().SetOid(indexkeycol_oid_t(2));

  // Primary, must be a BPLUSTREE due to ScanAscending usage
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::BPLUSTREE, true, true, false, true, options);

  return schema;
}

IndexSchema Builder::GetColumnNameIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back("attrelid", execution::sql::SqlTypeId::Integer, false,
                       parser::ColumnValueExpression(db, PgAttribute::COLUMN_TABLE_OID, PgAttribute::ATTRELID.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  columns.emplace_back("attname", execution::sql::SqlTypeId::Varchar, MAX_NAME_LENGTH, false,
                       parser::ColumnValueExpression(db, PgAttribute::COLUMN_TABLE_OID, PgAttribute::ATTNAME.oid_));
  columns.back().SetOid(indexkeycol_oid_t(2));

  // Unique, not primary
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, true, false, false, true, options);

  return schema;
}

IndexSchema Builder::GetTypeOidIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back("typoid", execution::sql::SqlTypeId::Integer, false,
                       parser::ColumnValueExpression(db, PgType::TYPE_TABLE_OID, PgType::TYPOID.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  // Primary
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, true, true, false, true, options);

  return schema;
}

IndexSchema Builder::GetTypeNameIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back("typnamespace", execution::sql::SqlTypeId::Integer, false,
                       parser::ColumnValueExpression(db, PgType::TYPE_TABLE_OID, PgType::TYPNAMESPACE.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  columns.emplace_back("typname", execution::sql::SqlTypeId::Varchar, MAX_NAME_LENGTH, false,
                       parser::ColumnValueExpression(db, PgType::TYPE_TABLE_OID, PgType::TYPNAME.oid_));
  columns.back().SetOid(indexkeycol_oid_t(2));

  // Unique, not primary
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, true, false, false, true, options);

  return schema;
}

IndexSchema Builder::GetTypeNamespaceIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back("typnamespace", execution::sql::SqlTypeId::Integer, false,
                       parser::ColumnValueExpression(db, PgType::TYPE_TABLE_OID, PgType::TYPNAMESPACE.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  // Not unique
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, false, false, false, true, options);

  return schema;
}

IndexSchema Builder::GetConstraintOidIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back(
      "conoid", execution::sql::SqlTypeId::Integer, false,
      parser::ColumnValueExpression(db, PgConstraint::CONSTRAINT_TABLE_OID, PgConstraint::CONOID.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  // Primary
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, true, true, false, true, options);

  return schema;
}

IndexSchema Builder::GetConstraintNameIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back(
      "connamespace", execution::sql::SqlTypeId::Integer, false,
      parser::ColumnValueExpression(db, PgConstraint::CONSTRAINT_TABLE_OID, PgConstraint::CONNAMESPACE.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  columns.emplace_back(
      "conname", execution::sql::SqlTypeId::Varchar, MAX_NAME_LENGTH, false,
      parser::ColumnValueExpression(db, PgConstraint::CONSTRAINT_TABLE_OID, PgConstraint::CONNAME.oid_));
  columns.back().SetOid(indexkeycol_oid_t(2));

  // Unique, not primary
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, true, false, false, true, options);

  return schema;
}

IndexSchema Builder::GetConstraintNamespaceIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back(
      "connamespace", execution::sql::SqlTypeId::Integer, false,
      parser::ColumnValueExpression(db, PgConstraint::CONSTRAINT_TABLE_OID, PgConstraint::CONNAMESPACE.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  // Not unique
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, false, false, false, true, options);

  return schema;
}

IndexSchema Builder::GetConstraintTableIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back(
      "conrelid", execution::sql::SqlTypeId::Integer, false,
      parser::ColumnValueExpression(db, PgConstraint::CONSTRAINT_TABLE_OID, PgConstraint::CONRELID.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  // Not unique
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, false, false, false, true, options);

  return schema;
}

IndexSchema Builder::GetConstraintIndexIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back(
      "conindid", execution::sql::SqlTypeId::Integer, false,
      parser::ColumnValueExpression(db, PgConstraint::CONSTRAINT_TABLE_OID, PgConstraint::CONINDID.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  // Not unique
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, false, false, false, true, options);

  return schema;
}

IndexSchema Builder::GetConstraintForeignTableIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back(
      "confrelid", execution::sql::SqlTypeId::Integer, false,
      parser::ColumnValueExpression(db, PgConstraint::CONSTRAINT_TABLE_OID, PgConstraint::CONFRELID.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  // Not unique
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, false, false, false, true, options);

  return schema;
}

IndexSchema Builder::GetLanguageOidIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back("lanoid", execution::sql::SqlTypeId::Integer, false,
                       parser::ColumnValueExpression(db, PgLanguage::LANGUAGE_TABLE_OID, PgLanguage::LANOID.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  // Primary
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, true, true, false, true, options);

  return schema;
}

IndexSchema Builder::GetLanguageNameIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back("lanname", execution::sql::SqlTypeId::Varchar, MAX_NAME_LENGTH, false,
                       parser::ColumnValueExpression(db, PgLanguage::LANGUAGE_TABLE_OID, PgLanguage::LANNAME.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  // Unique, not primary
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, true, false, false, true, options);

  return schema;
}

Schema Builder::GetStatisticTableSchema() {
  std::vector<Schema::Column> columns;

  columns.emplace_back("starelid", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgStatistic::STARELID.oid_);

  columns.emplace_back("staattnum", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgStatistic::STAATTNUM.oid_);

  columns.emplace_back("stanumrows", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgStatistic::STA_NUMROWS.oid_);

  columns.emplace_back("stanonnullrows", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgStatistic::STA_NONNULLROWS.oid_);

  columns.emplace_back("stadistinctrows", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgStatistic::STA_DISTINCTROWS.oid_);

  columns.emplace_back("statopk", execution::sql::SqlTypeId::Varbinary, true,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varbinary));
  columns.back().SetOid(PgStatistic::STA_TOPK.oid_);

  columns.emplace_back("stahistogram", execution::sql::SqlTypeId::Varbinary, true,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varbinary));
  columns.back().SetOid(PgStatistic::STA_HISTOGRAM.oid_);

  return Schema(columns);
}

Schema Builder::GetProcTableSchema() {
  std::vector<Schema::Column> columns;

  columns.emplace_back("prooid", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgProc::PROOID.oid_);

  columns.emplace_back("proname", execution::sql::SqlTypeId::Varchar, MAX_NAME_LENGTH, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varchar));
  columns.back().SetOid(PgProc::PRONAME.oid_);

  columns.emplace_back("pronamespace", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgProc::PRONAMESPACE.oid_);

  columns.emplace_back("prolang", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgProc::PROLANG.oid_);

  columns.emplace_back("procost", execution::sql::SqlTypeId::Double, true,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Double));
  columns.back().SetOid(PgProc::PROCOST.oid_);

  columns.emplace_back("prorows", execution::sql::SqlTypeId::Double, true,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Double));
  columns.back().SetOid(PgProc::PROROWS.oid_);

  columns.emplace_back("provariadic", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgProc::PROVARIADIC.oid_);

  columns.emplace_back("proisagg", execution::sql::SqlTypeId::Boolean, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Boolean));
  columns.back().SetOid(PgProc::PROISAGG.oid_);

  columns.emplace_back("proiswindow", execution::sql::SqlTypeId::Boolean, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Boolean));
  columns.back().SetOid(PgProc::PROISWINDOW.oid_);

  columns.emplace_back("proisstrict", execution::sql::SqlTypeId::Boolean, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Boolean));
  columns.back().SetOid(PgProc::PROISSTRICT.oid_);

  columns.emplace_back("proretset", execution::sql::SqlTypeId::Boolean, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Boolean));
  columns.back().SetOid(PgProc::PRORETSET.oid_);

  columns.emplace_back("provolatile", execution::sql::SqlTypeId::TinyInt, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::TinyInt));
  columns.back().SetOid(PgProc::PROVOLATILE.oid_);

  columns.emplace_back("pronargs", execution::sql::SqlTypeId::SmallInt, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::SmallInt));
  columns.back().SetOid(PgProc::PRONARGS.oid_);

  columns.emplace_back("pronargdefaults", execution::sql::SqlTypeId::SmallInt, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::SmallInt));
  columns.back().SetOid(PgProc::PRONARGDEFAULTS.oid_);

  columns.emplace_back("prorettype", execution::sql::SqlTypeId::Integer, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Integer));
  columns.back().SetOid(PgProc::PRORETTYPE.oid_);

  columns.emplace_back("proargtypes", execution::sql::SqlTypeId::Varbinary, 4096, true,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varbinary));
  columns.back().SetOid(PgProc::PROARGTYPES.oid_);

  columns.emplace_back("proallargtypes", execution::sql::SqlTypeId::Varbinary, 4096, true,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varbinary));
  columns.back().SetOid(PgProc::PROALLARGTYPES.oid_);

  columns.emplace_back("proargmodes", execution::sql::SqlTypeId::Varbinary, 4096, true,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varbinary));
  columns.back().SetOid(PgProc::PROARGMODES.oid_);

  columns.emplace_back("proargdefaults", execution::sql::SqlTypeId::Varbinary, 4096, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varbinary));
  columns.back().SetOid(PgProc::PROARGDEFAULTS.oid_);

  columns.emplace_back("proargnames", execution::sql::SqlTypeId::Varbinary, 4096, true,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varbinary));
  columns.back().SetOid(PgProc::PROARGNAMES.oid_);

  columns.emplace_back("prosrc", execution::sql::SqlTypeId::Varchar, 4096, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varchar));
  columns.back().SetOid(PgProc::PROSRC.oid_);

  columns.emplace_back("proconfig", execution::sql::SqlTypeId::Varbinary, 4096, false,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::Varbinary));
  columns.back().SetOid(PgProc::PROCONFIG.oid_);

  columns.emplace_back("ctx_pointer", execution::sql::SqlTypeId::BigInt, true,
                       parser::ConstantValueExpression(execution::sql::SqlTypeId::BigInt));
  columns.back().SetOid(PgProc::PRO_CTX_PTR.oid_);

  return Schema(columns);
}

IndexSchema Builder::GetProcOidIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back("prooid", execution::sql::SqlTypeId::Integer, false,
                       parser::ColumnValueExpression(db, PgProc::PRO_TABLE_OID, PgProc::PROOID.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  // Primary
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::HASHMAP, true, true, false, true, options);

  return schema;
}

IndexSchema Builder::GetProcNameIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back("pronamespace", execution::sql::SqlTypeId::Integer, false,
                       parser::ColumnValueExpression(db, PgProc::PRO_TABLE_OID, PgProc::PRONAMESPACE.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  columns.emplace_back("proname", execution::sql::SqlTypeId::Varchar, MAX_NAME_LENGTH, false,
                       parser::ColumnValueExpression(db, PgProc::PRO_TABLE_OID, PgProc::PRONAME.oid_));
  columns.back().SetOid(indexkeycol_oid_t(2));

  // Non-Unique, not primary
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::BPLUSTREE, false, false, false, false, options);

  return schema;
}

IndexSchema Builder::GetStatisticOidIndexSchema(db_oid_t db) {
  std::vector<IndexSchema::Column> columns;

  columns.emplace_back("starelid", execution::sql::SqlTypeId::Integer, false,
                       parser::ColumnValueExpression(db, PgStatistic::STATISTIC_TABLE_OID, PgStatistic::STARELID.oid_));
  columns.back().SetOid(indexkeycol_oid_t(1));

  columns.emplace_back(
      "staattnum", execution::sql::SqlTypeId::Integer, false,
      parser::ColumnValueExpression(db, PgStatistic::STATISTIC_TABLE_OID, PgStatistic::STAATTNUM.oid_));
  columns.back().SetOid(indexkeycol_oid_t(2));

  // Primary
  catalog::IndexOptions options;
  IndexSchema schema(columns, storage::index::IndexType::BPLUSTREE, true, true, false, true, options);

  return schema;
}

storage::index::Index *Builder::BuildUniqueIndex(const IndexSchema &key_schema, index_oid_t oid) {
  NOISEPAGE_ASSERT(key_schema.Unique(), "KeySchema must represent a unique index.");
  storage::index::IndexBuilder index_builder;
  index_builder.SetKeySchema(key_schema);
  return index_builder.Build();
}

storage::index::Index *Builder::BuildLookupIndex(const IndexSchema &key_schema, index_oid_t oid) {
  NOISEPAGE_ASSERT(!(key_schema.Unique()), "KeySchema must represent a non-unique index.");
  storage::index::IndexBuilder index_builder;
  index_builder.SetKeySchema(key_schema);
  return index_builder.Build();
}

}  // namespace noisepage::catalog::postgres
