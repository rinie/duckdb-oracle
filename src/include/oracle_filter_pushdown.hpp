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

class LogicalGet;

class OracleFilterPushdown {
public:
	//! Whether an expression can be rendered as an Oracle WHERE clause fragment
	static bool CanPushExpressionDown(ClientContext &context, const LogicalGet &get, Expression &expr);

	//! Transform DuckDB table filters into Oracle WHERE clause fragment
	static string TransformFilters(const vector<column_t> &column_ids,
	                               optional_ptr<TableFilterSet> filters,
	                               const vector<string> &names);
};

} // namespace duckdb
