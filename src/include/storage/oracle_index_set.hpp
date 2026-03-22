//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/oracle_index_set.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/oracle_catalog_set.hpp"

namespace duckdb {

class OracleIndexSet : public OracleInSchemaSet {
public:
	explicit OracleIndexSet(OracleSchemaEntry &schema, unique_ptr<OracleResultSlice> index_result = nullptr);

protected:
	void LoadEntries(ClientContext &context, OracleTransaction &transaction) override;

private:
	//! Populate index entries from a contiguous range [start, end) of a result set.
	//! Shared by both the standard query path and any pre-loaded slice path.
	void PopulateFromResult(OracleResult &result, idx_t start, idx_t end);

	unique_ptr<OracleResultSlice> index_result;
};

} // namespace duckdb
