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
	//! Build index entries from a contiguous range of rows in an OracleResult.
	//! Shared by both the pre-loaded and the standard Oracle query paths.
	void PopulateFromResult(OracleResult &result, idx_t start, idx_t end);

	unique_ptr<OracleResultSlice> index_result;
};

} // namespace duckdb
