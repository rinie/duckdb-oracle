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
	explicit OracleTableSet(OracleSchemaEntry &schema);

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

	//! ALL_* queries — bind :owner and :table_name
	static string GetColumnsQuery();
	static string GetConstraintsQuery();
	//! ALL_* name query — bind :owner
	static string GetTableNamesQuery();

	//! USER_* queries — bind :table_name only (no :owner needed)
	static string GetUserColumnsQuery();
	static string GetUserConstraintsQuery();
	//! USER_* name query — no bind params needed
	static string GetUserTableNamesQuery();

	//! DBA_* queries — bind :owner and :table_name
	static string GetDbaColumnsQuery();
	static string GetDbaConstraintsQuery();
	//! DBA_* name query — bind :owner
	static string GetDbaTableNamesQuery();

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

private:
	string GetAlterTablePrefix(ClientContext &context, OracleTransaction &transaction,
	                            const string &name);
	string GetAlterTablePrefix(const string &name, optional_ptr<CatalogEntry> entry);
};

} // namespace duckdb
