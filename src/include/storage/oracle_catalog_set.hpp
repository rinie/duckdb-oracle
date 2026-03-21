//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/oracle_catalog_set.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/mutex.hpp"
#include "oracle_result.hpp"

namespace duckdb {
class OracleCatalog;
class OracleSchemaEntry;
class OracleTransaction;


class OracleCatalogSet {
public:
	explicit OracleCatalogSet(OracleCatalog &catalog);
	virtual ~OracleCatalogSet() = default;

public:
	optional_ptr<CatalogEntry> GetEntry(ClientContext &context, OracleTransaction &transaction,
	                                    const string &name);
	void Scan(ClientContext &context, OracleTransaction &transaction,
	          const std::function<void(CatalogEntry &)> &callback);
	optional_ptr<CatalogEntry> CreateEntry(OracleTransaction &transaction,
	                                        shared_ptr<CatalogEntry> entry);
	void DropEntry(OracleTransaction &transaction, DropInfo &info);
	void ClearEntries();

protected:
	// Called from within LoadEntries/ReloadEntry while entry_lock is already held.
	optional_ptr<CatalogEntry> CreateEntryInternal(shared_ptr<CatalogEntry> entry);
	// Mark a cached entry as a name-only stub that needs ReloadEntry before use.
	// Called by LoadEntries implementations that defer column loading.
	void MarkAsStub(const string &name);

	virtual void LoadEntries(ClientContext &context, OracleTransaction &transaction) = 0;
	virtual bool SupportReload() const {
		return false;
	}
	virtual optional_ptr<CatalogEntry> ReloadEntry(OracleTransaction &transaction,
	                                                const string &name) {
		return nullptr;
	}

protected:
	OracleCatalog &catalog;
	mutex entry_lock;
	case_insensitive_map_t<shared_ptr<CatalogEntry>> entries;
	case_insensitive_set_t stub_names; // name-only stubs pending column load
	bool is_loaded = false;
};

class OracleInSchemaSet : public OracleCatalogSet {
public:
	OracleInSchemaSet(OracleSchemaEntry &schema, bool loaded = false);

protected:
	OracleSchemaEntry &schema;
};

} // namespace duckdb
