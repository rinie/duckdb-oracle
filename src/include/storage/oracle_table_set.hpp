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

	//! ALL_* queries: bind :owner and :table_name
	static string GetColumnsQuery();
	static string GetConstraintsQuery();
	//! ALL_* queries: bind :owner only (schema-wide bulk load)
	static string GetSchemaColumnsQuery();
	static string GetSchemaConstraintsQuery();

	//! USER_* variants (no :owner needed — implicitly the connected user)
	//! Single-table: bind :table_name only
	static string GetUserColumnsQuery();
	static string GetUserConstraintsQuery();
	//! USER_* schema-wide: no bind params needed
	static string GetUserSchemaColumnsQuery();
	static string GetUserSchemaConstraintsQuery();

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
	                   idx_t con_start, idx_t con_end);

private:
	string GetAlterTablePrefix(ClientContext &context, OracleTransaction &transaction,
	                            const string &name);
	string GetAlterTablePrefix(const string &name, optional_ptr<CatalogEntry> entry);

protected:
	unique_ptr<OracleResultSlice> table_result;
	unique_ptr<OracleResultSlice> constraint_result;
};

} // namespace duckdb
