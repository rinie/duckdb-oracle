#include "duckdb/main/client_context.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "storage/oracle_catalog_set.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_transaction.hpp"
#include "storage/oracle_schema_entry.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"

namespace duckdb {

OracleCatalogSet::OracleCatalogSet(OracleCatalog &catalog) : catalog(catalog) {
}

optional_ptr<CatalogEntry> OracleCatalogSet::GetEntry(ClientContext &context,
                                                        OracleTransaction &transaction,
                                                        const string &name) {
	lock_guard<mutex> l(entry_lock);
	// 1. Cache hit — fastest path (works whether bulk-loaded or not)
	auto it = entries.find(name);
	if (it != entries.end()) {
		// If this is a name-only stub, upgrade it to a full entry now.
		if (stub_names.count(name) && SupportReload()) {
			auto full = ReloadEntry(transaction, name);
			if (full) {
				stub_names.erase(name);
			}
			return full;
		}
		return it->second.get();
	}
	if (!is_loaded) {
		if (SupportReload()) {
			// 2. Fast path: single-entry query instead of bulk ALL_TAB_COLUMNS scan.
			// Skips the expensive LoadEntries on first individual table access.
			return ReloadEntry(transaction, name);
		}
		// 3. No reload support (e.g. schema set): fall back to bulk load.
		is_loaded = true;
		LoadEntries(context, transaction);
		auto entry = entries.find(name);
		if (entry != entries.end()) {
			return entry->second.get();
		}
	}
	// 4. Entry missing after bulk load or not in cache after fast-path.
	// Fallback ReloadEntry handles tables created after initial load and
	// preserves the same behaviour as the original code for this case.
	if (SupportReload()) {
		return ReloadEntry(transaction, name);
	}
	return nullptr;
}

void OracleCatalogSet::Scan(ClientContext &context, OracleTransaction &transaction,
                              const std::function<void(CatalogEntry &)> &callback) {
	lock_guard<mutex> l(entry_lock);
	if (!is_loaded) {
		is_loaded = true;
		LoadEntries(context, transaction);
	}
	for (auto &entry : entries) {
		callback(*entry.second);
	}
}

optional_ptr<CatalogEntry> OracleCatalogSet::CreateEntryInternal(shared_ptr<CatalogEntry> entry) {
	auto name = entry->name.GetIdentifierName();
	auto result = entry.get();
	entries[name] = std::move(entry);
	return result;
}

optional_ptr<CatalogEntry> OracleCatalogSet::CreateEntry(OracleTransaction &transaction,
                                                           shared_ptr<CatalogEntry> entry) {
	lock_guard<mutex> l(entry_lock);
	return CreateEntryInternal(std::move(entry));
}

void OracleCatalogSet::DropEntry(OracleTransaction &transaction, DropInfo &info) {
	lock_guard<mutex> l(entry_lock);
	auto entry = entries.find(info.GetQualifiedName().Name().GetIdentifierName());
	if (entry == entries.end()) {
		if (info.if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw BinderException("Cannot drop entry \"%s\": does not exist",
			                      info.GetQualifiedName().Name());
		}
		return;
	}
	entries.erase(entry);
}

void OracleCatalogSet::ClearEntries() {
	lock_guard<mutex> l(entry_lock);
	entries.clear();
	stub_names.clear();
	is_loaded = false;
}

void OracleCatalogSet::MarkAsStub(const string &name) {
	stub_names.insert(name);
}

// ---------------------------------------------------------------------------
// OracleInSchemaSet
// ---------------------------------------------------------------------------

OracleInSchemaSet::OracleInSchemaSet(OracleSchemaEntry &schema, bool loaded)
    : OracleCatalogSet(schema.ParentCatalog().Cast<OracleCatalog>()), schema(schema) {
	if (loaded) {
		is_loaded = true;
	}
}

} // namespace duckdb
