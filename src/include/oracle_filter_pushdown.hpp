//===----------------------------------------------------------------------===//
//                         DuckDB
//
// oracle_filter_pushdown.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/table_filter_set.hpp"

namespace duckdb {

class OracleFilterPushdown {
public:
	//! Transform DuckDB table filters into Oracle WHERE clause fragment
	static string TransformFilters(const vector<column_t> &column_ids,
	                               optional_ptr<TableFilterSet> filters,
	                               const vector<string> &names);
};

} // namespace duckdb
