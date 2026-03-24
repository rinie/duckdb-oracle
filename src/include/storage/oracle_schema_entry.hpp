//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/oracle_schema_entry.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "storage/oracle_table_set.hpp"
#include "storage/oracle_index_set.hpp"
#include "storage/oracle_type_set.hpp"
#include "storage/oracle_view_entry.hpp"
#include <unordered_map>

namespace duckdb {
class OracleTransaction;

class OracleSchemaEntry : public SchemaCatalogEntry {
public:
	OracleSchemaEntry(Catalog &catalog, CreateSchemaInfo &info);

	//! The actual Oracle schema (owner) name used in Oracle queries.
	string oracle_name;

public:
	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction,
	                                        BoundCreateTableInfo &info) override;
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction,
	                                           CreateFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction,
	                                        CreateIndexInfo &info,
	                                        TableCatalogEntry &table) override;
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction,
	                                       CreateViewInfo &info) override;
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction,
	                                           CreateSequenceInfo &info) override;
	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
	                                                CreateTableFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
	                                               CreateCopyFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
	                                                 CreatePragmaFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction,
	                                            CreateCollationInfo &info) override;
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction,
	                                       CreateTypeInfo &info) override;
	void Alter(CatalogTransaction transaction, AlterInfo &info) override;
	void Scan(ClientContext &context, CatalogType type,
	          const std::function<void(CatalogEntry &)> &callback) override;
	void Scan(CatalogType type,
	          const std::function<void(CatalogEntry &)> &callback) override;
	void DropEntry(ClientContext &context, DropInfo &info) override;
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction,
	                                        const EntryLookupInfo &lookup_info) override;

	static bool SchemaIsInternal(const string &name);
	//! SQL literal list of internal owners for NOT IN (...) clauses, e.g. "'SYS','SYSTEM',..."
	static string InternalOwnersSQL();
	//! True when this schema's oracle_name matches the connected user (enables USER_* views)
	bool IsCurrentUserSchema() const;

private:
	void TryDropEntry(ClientContext &context, CatalogType catalog_type, const string &name);
	OracleCatalogSet &GetCatalogSet(CatalogType type);

	void AlterTable(OracleTransaction &transaction, RenameTableInfo &info);
	void AlterTable(OracleTransaction &transaction, RenameColumnInfo &info);
	void AlterTable(OracleTransaction &transaction, AddColumnInfo &info);
	void AlterTable(OracleTransaction &transaction, RemoveColumnInfo &info);

private:
	OracleTableSet tables;
	OracleIndexSet indexes;
	OracleTypeSet types;
	//! Heap-allocated VIEW_ENTRY stubs, keyed by view name.
	//! DuckDBViewsInit stores reference<CatalogEntry> pointers into these during
	//! Scan(VIEW_ENTRY, …) and reads them back later in DuckDBViewsFunction — so
	//! the stubs must outlive the Scan call.  Stale entries for dropped views are
	//! harmless: they are never returned because the underlying OracleTableSet no
	//! longer contains them.
	mutable std::unordered_map<std::string, unique_ptr<OracleViewEntry>> view_stub_cache_;
};

} // namespace duckdb
