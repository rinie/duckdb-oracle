//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/oracle_table_entry.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "oracle_utils.hpp"

namespace duckdb {
class OracleCatalog;
class OracleSchemaEntry;

struct OracleTableInfo {
	OracleTableInfo(OracleSchemaEntry &schema, const string &table_name);
	OracleTableInfo(const string &schema_name, const string &table_name);

	const string &GetTableName() const {
		return create_info->table;
	}

	optional_ptr<OracleSchemaEntry> schema;
	string schema_name;
	unique_ptr<CreateTableInfo> create_info;
	vector<OracleType> oracle_types;
	vector<string> oracle_names; // column names as stored in data dictionary
	idx_t approx_num_rows = 0;
};

class OracleTableEntry : public TableCatalogEntry {
public:
	OracleTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, OracleTableInfo &info);
	OracleTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info);

public:
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;

	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj,
	                           LogicalUpdate &update, ClientContext &context) override;

	//! Return the copy format for this table (not applicable for Oracle, always returns NONE)
	string GetCopyFormat(ClientContext &context) const;

	//! Lookup a column index by name (returns invalid index if not found)
	PhysicalIndex GetColumnIndex(const string &name, bool silence_error = false) const;

	//! The actual Oracle schema (owner) name for Oracle DML/scan queries.
	//! Equals schema.name for normal schemas; equals the default schema for the "main" alias.
	string oracle_schema_name;
	//! The column names as stored in Oracle data dictionary (may differ in case)
	vector<string> oracle_names;
	vector<OracleType> oracle_types;
	idx_t approx_num_rows = 0;
};

} // namespace duckdb
