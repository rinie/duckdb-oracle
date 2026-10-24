//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/oracle_index_entry.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"

namespace duckdb {

class OracleIndexEntry : public IndexCatalogEntry {
public:
	OracleIndexEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateIndexInfo &info);
	Identifier GetSchemaName() const override;
	Identifier GetTableName() const override;

private:
	string table_name;
};

} // namespace duckdb
