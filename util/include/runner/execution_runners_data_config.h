#pragma once

#include <utility>
#include <vector>

#include "execution/sql/sql.h"

namespace noisepage::runner {

/**
 * Configuration for mini-runner generated data.
 * Stores all the parameters for generating tables.
 */
class ExecutionRunnersDataConfig {
 public:
  /** Distribution of table column types */
  std::vector<std::vector<execution::sql::SqlTypeId>> table_type_dists_ = {
      {execution::sql::SqlTypeId::Integer},
      {execution::sql::SqlTypeId::BigInt},
      {execution::sql::SqlTypeId::Varchar},
      {execution::sql::SqlTypeId::Integer, execution::sql::SqlTypeId::Double, execution::sql::SqlTypeId::BigInt},
      {execution::sql::SqlTypeId::Integer, execution::sql::SqlTypeId::Varchar}};

  /**
   * Distribution of table columns
   *
   * Describes a set of table column distributions to be used when creating
   * data for the mini-runners. The explanation for this is best illustrated
   * with an example.
   *
   * Consider table_col_dists_ = {{{1, 2, 3},...},...}.
   * Now note that y = table_col_dists_[i=0][j=0] = {1, 2, 3}
   *
   * This means that a table [t] created from [y] is comprised of three column
   * types (integer, real, and bigint) based on table_type_dists_[i=0].
   * Furthermore, the number of columns in table [t] can be obtained by summing
   * up all the numbers in [y] which is 6 based on the fact that [t] has
   * 1 INTEGER, 2 DECIMALS, and 3 BIGINTS (y[k] is the number of columns of
   * type table_type_dists_[i=0][k]).
   */
  std::vector<std::vector<std::vector<uint32_t>>> table_col_dists_ = {
      {{15}}, {{15}}, {{5}}, {{0, 15, 0}, {3, 12, 0}, {7, 8, 0}, {11, 4, 0}}, {{1, 4}, {2, 3}, {3, 2}, {4, 1}}};

  /**
   * Distribution of row numbers of tables to create.
   *
   * Note that for each row number, we create multiple tables, varying the
   * cardinality in powers of 2. For instance, when creating a table of
   * 100 tuples, we create tables of 100 tuples with cardinality 1, 2, 4,
   * 8, 16, 32, 64, and 100.
   */
  std::vector<uint32_t> table_row_nums_ = {1,    3,    5,     7,     10,    50,     100,    200,    500,    1000,
                                           2000, 5000, 10000, 20000, 50000, 100000, 200000, 300000, 500000, 1000000};

  /**
   * Parameter controls number of columns extracted from base tables (for integer, real, and bigint).
   */
  std::vector<uint32_t> sweep_col_nums_ = {1, 3, 5, 7, 9, 11, 13, 15};

  /**
   * Parameter controls number of columns extracted from base tables (for varchar).
   * Note: This is different than other types due to size concerns.
   */
  std::vector<uint32_t> sweep_varchar_col_nums_ = {1, 3, 5};

  /**
   * Parameter controls distribution of mixed (integer, real/bigint) for scans
   */
  std::vector<std::pair<uint32_t, uint32_t>> sweep_scan_mixed_dist_ = {{3, 12}, {7, 8}, {11, 4}};

  /**
   * Parameter controls distribution of mixed (integer, varchar) for scans
   */
  std::vector<std::pair<uint32_t, uint32_t>> sweep_scan_mixed_varchar_dist_ = {{2, 3}, {3, 2}, {4, 1}};

  /**
   * Parameter controls number of keys to be used in mini-runner index lookups (for integer, real, and bigint).
   */
  std::vector<uint32_t> sweep_index_col_nums_ = {1, 2, 4, 8, 15};

  /**
   * Parameter controls number of keys for UPDATE mini-runners
   */
  std::vector<uint32_t> sweep_update_index_col_nums_ = {1};

  /**
   * Parameter controls number of columns to update
   */
  std::vector<uint32_t> sweep_update_col_nums_ = {1, 2, 4, 8, 12};

  /**
   * Parameter controls number of keys to be used in mini-runner index lookups (for varchar).
   * Note: This is different than other types due to size concerns.
   */
  std::vector<uint32_t> sweep_varchar_index_col_nums_ = {1, 2, 4};

  /**
   * Parameter controls size of index scan lookups.
   */
  std::vector<uint32_t> sweep_index_lookup_sizes_ = {1,   10,   20,    30,    40,    50,    75,
                                                     100, 1000, 10000, 20000, 30000, 40000, 50000};

  /*
   * Parameter controls # threads to sweep for building index.
   * 0 is a special argument to indicate serial build.
   */
  std::vector<uint32_t> sweep_index_create_threads_ = {0, 1, 2, 4, 8, 16};

  /**
   * Parameter controls number of insert tuples
   */
  std::vector<uint32_t> sweep_insert_row_nums_ = {1, 10, 100, 200, 500, 1000, 2000, 5000, 10000};

  /**
   * Parameter controls distribution of mixed (integer, real) tuples.
   */
  std::vector<std::pair<uint32_t, uint32_t>> sweep_insert_mixed_dist_ = {{1, 14}, {3, 12}, {5, 10}, {7, 8},
                                                                         {9, 6},  {11, 4}, {13, 2}};

  /**
   * Gets valid table row numbers less than or equal to a certain limit
   * @param limit Largest size of possible table
   * @returns Elements of table_row_nums <= limit
   */
  std::vector<uint32_t> GetRowNumbersWithLimit(int64_t limit) const {
    std::vector<uint32_t> rows;
    for (auto row : table_row_nums_) {
      if (row <= limit) {
        rows.push_back(row);
      }
    }
    return rows;
  }
};

};  // namespace noisepage::runner
