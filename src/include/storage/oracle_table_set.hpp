//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/oracle_table_set.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/oracle_catalog_set.hpp"
#include "storage/oracle_table_entry.hpp"

namespace duckdb {
struct CreateTableInfo;
class OracleConnection;
class OracleResult;
class OracleSchemaEntry;

class OracleTableSet : public OracleInSchemaSet {
public:
	explicit OracleTableSet(OracleSchemaEntry &schema,
	                         unique_ptr<OracleResultSlice> tables = nullptr,
	                         unique_ptr<OracleResultSlice> constraints = nullptr);

public:
	optional_ptr<CatalogEntry> CreateTable(OracleTransaction &transaction,
	                                        BoundCreateTableInfo &info);

	static unique_ptr<OracleTableInfo> GetTableInfo(OracleTransaction &transaction,
	                                                  OracleSchemaEntry &schema,
	                                                  const string &table_name);
	static unique_ptr<OracleTableInfo> GetTableInfo(ClientContext &context,
	                                                  OracleConnection &connection,
	                                                  const string &schema_name,
	                                                  const string &table_name);
	optional_ptr<CatalogEntry> ReloadEntry(OracleTransaction &transaction,
	                                        const string &table_name) override;

	void AlterTable(ClientContext &context, OracleTransaction &transaction,
	                AlterTableInfo &info);

	//! SQL to fetch column info for a specific owner+table (use :owner / :table_name bind params)
	static string GetColumnsQuery();
	//! SQL to fetch constraint info for a specific owner+table (use :owner / :table_name bind params)
	static string GetConstraintsQuery();

	//! SQL to fetch column info for ALL tables in a schema (use :owner bind param only)
	static string GetSchemaColumnsQuery();
	//! SQL to fetch constraint info for ALL tables in a schema (use :owner bind param only)
	static string GetSchemaConstraintsQuery();

	//! USER_* view variants (no :owner bind needed — faster for the connected user's schema)
	static string GetUserColumnsQuery();
	static string GetUserConstraintsQuery();
	static string GetUserSchemaColumnsQuery();
	static string GetUserSchemaConstraintsQuery();

	//! Row-count queries (separate lightweight queries, no JOIN needed)
	static string GetSchemaRowCountsQuery();
	static string GetUserSchemaRowCountsQuery();

protected:
	void LoadEntries(ClientContext &context, OracleTransaction &transaction) override;
	bool SupportReload() const override {
		return true;
	}

	void AlterTable(ClientContext &context, OracleTransaction &transaction,
	                RenameTableInfo &info);
	void AlterTable(ClientContext &context, OracleTransaction &transaction,
	                RenameColumnInfo &info);
	void AlterTable(ClientContext &context, OracleTransaction &transaction,
	                AddColumnInfo &info);
	void AlterTable(ClientContext &context, OracleTransaction &transaction,
	                RemoveColumnInfo &info);

	static void AddColumn(OracleResult &result, idx_t row,
	                       OracleTableInfo &table_info);
	static void AddConstraint(const string &constraint_type,
	                           const string &column_name,
	                           OracleTableInfo &table_info,
	                           case_insensitive_map_t<vector<string>> &constraint_cols,
	                           case_insensitive_map_t<string> &constraint_types);

	void CreateEntries(OracleTransaction &transaction, OracleResult &col_result,
	                   OracleResult &con_result, idx_t col_start, idx_t col_end,
	                   idx_t con_start, idx_t con_end,
	                   const case_insensitive_map_t<int64_t> &row_counts = {});

private:
	string GetAlterTablePrefix(ClientContext &context, OracleTransaction &transaction,
	                            const string &name);
	string GetAlterTablePrefix(const string &name, optional_ptr<CatalogEntry> entry);

protected:
	unique_ptr<OracleResultSlice> table_result;
	unique_ptr<OracleResultSlice> constraint_result;
};

} // namespace duckdb
