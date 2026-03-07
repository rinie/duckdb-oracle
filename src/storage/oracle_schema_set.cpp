#include "storage/oracle_schema_set.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_transaction.hpp"
#include "storage/oracle_schema_entry.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"

namespace duckdb {

OracleSchemaSet::OracleSchemaSet(OracleCatalog &catalog, const string &schema_to_load)
    : OracleCatalogSet(catalog), schema_to_load(schema_to_load) {
}

void OracleSchemaSet::LoadEntries(ClientContext &context, OracleTransaction &transaction) {
	string query;
	if (!schema_to_load.empty()) {
		// Load only the requested schema
		query = StringUtil::Format(
		    "SELECT DISTINCT owner FROM all_tables WHERE owner = %s "
		    "UNION SELECT DISTINCT owner FROM all_views WHERE owner = %s",
		    OracleUtils::WriteLiteral(StringUtil::Upper(schema_to_load)),
		    OracleUtils::WriteLiteral(StringUtil::Upper(schema_to_load)));
	} else {
		// Load all visible schemas/owners
		query =
		    "SELECT DISTINCT owner FROM all_tables "
		    "UNION SELECT DISTINCT owner FROM all_views "
		    "ORDER BY 1";
	}

	auto result = transaction.Query(query);
	if (result) {
		for (idx_t row = 0; row < result->Count(); row++) {
			auto owner = result->GetString(row, 0);
			if (OracleSchemaEntry::SchemaIsInternal(owner)) {
				continue;
			}
			CreateSchemaInfo info;
			info.schema = owner;
			info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
			auto entry = make_shared_ptr<OracleSchemaEntry>(catalog, info);
			entries[owner] = std::move(entry);
		}
	}

	// Always register "main" as a DuckDB-visible alias for the default Oracle schema so
	// that the DuckDB UI (which sets search_path = <catalog>.main on attach) finds a
	// schema named "main" in ScanSchemas and can list its tables correctly.
	// We do this unconditionally: even if the schema has no tables/views yet (empty
	// schema, synonyms-only, or query failure) the alias must still exist so that
	// LookupSchema("main") succeeds and SET schema works.
	const auto &ds = catalog.GetDefaultSchema();
	if (!ds.empty() && !StringUtil::CIEquals(ds, "main")) {
		CreateSchemaInfo main_info;
		main_info.schema = "main";
		main_info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
		auto main_entry = make_shared_ptr<OracleSchemaEntry>(catalog, main_info);
		main_entry->oracle_name = ds;
		entries["main"] = std::move(main_entry);
	}
}

} // namespace duckdb
