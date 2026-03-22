//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/oracle_catalog.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/enums/access_mode.hpp"
#include "oracle_connection.hpp"
#include "storage/oracle_schema_set.hpp"
#include "storage/oracle_connection_pool.hpp"

namespace duckdb {
class OracleCatalog;
class OracleSchemaEntry;

class OracleCatalog : public Catalog {
public:
	explicit OracleCatalog(AttachedDatabase &db_p, string connection_string,
	                        string attach_path, AccessMode access_mode,
	                        string schema_to_load,
	                        OracleIsolationLevel isolation_level,
	                        OraclePrivilegeLevel privilege_level,
	                        ClientContext &context);
	~OracleCatalog();

	string connection_string;
	string attach_path;
	AccessMode access_mode;
	OracleIsolationLevel isolation_level;
	OraclePrivilegeLevel privilege_level;

public:
	void Initialize(bool load_builtin) override;
	string GetCatalogType() override {
		return "oracle";
	}
	string GetDefaultSchema() const override {
		return default_schema;
	}

	static string GetConnectionString(ClientContext &context, const string &attach_path,
	                                   string secret_name);

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction,
	                                         CreateSchemaInfo &info) override;

	void ScanSchemas(ClientContext &context,
	                  std::function<void(SchemaCatalogEntry &)> callback) override;

	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction,
	                                               const EntryLookupInfo &schema_lookup,
	                                               OnEntryNotFound if_not_found) override;

	PhysicalOperator &PlanCreateTableAs(ClientContext &context,
	                                     PhysicalPlanGenerator &planner,
	                                     LogicalCreateTable &op,
	                                     PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner,
	                              LogicalInsert &op,
	                              optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner,
	                              LogicalDelete &op, PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner,
	                              LogicalUpdate &op, PhysicalOperator &plan) override;
	PhysicalOperator &PlanMergeInto(ClientContext &context,
	                                 PhysicalPlanGenerator &planner,
	                                 LogicalMergeInto &op,
	                                 PhysicalOperator &plan) override;

	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt,
	                                             TableCatalogEntry &table,
	                                             unique_ptr<LogicalOperator> plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;

	OracleVersion GetOracleVersion() const {
		return version;
	}

	OraclePrivilegeLevel GetPrivilegeLevel() const {
		return privilege_level;
	}

	static void MaterializeOracleScans(PhysicalOperator &op);
	static bool IsOracleScan(const string &name);

	bool InMemory() override;
	string GetDBPath() override;

	OracleConnectionPool &GetConnectionPool() {
		return connection_pool;
	}

	void ClearCache();

	CatalogLookupBehavior CatalogTypeLookupRule(CatalogType type) const override {
		switch (type) {
		case CatalogType::INDEX_ENTRY:
		case CatalogType::TABLE_ENTRY:
		case CatalogType::TYPE_ENTRY:
		case CatalogType::VIEW_ENTRY:
			return CatalogLookupBehavior::STANDARD;
		default:
			return CatalogLookupBehavior::NEVER_LOOKUP;
		}
	}

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;

private:
	OracleVersion version;
	OracleSchemaSet schemas;
	OracleConnectionPool connection_pool;
	string default_schema;
};

} // namespace duckdb
