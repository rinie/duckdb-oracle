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
	if (!result) {
		return;
	}

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

	// Register "main" as a DuckDB-visible alias for the default Oracle schema so that
	// the DuckDB UI (which sets search_path = <catalog>.main on attach) finds a schema
	// named "main" in ScanSchemas and can list its tables correctly.
	const auto &ds = catalog.GetDefaultSchema();
	if (!ds.empty() && !StringUtil::CIEquals(ds, "main") && entries.count(ds)) {
		CreateSchemaInfo main_info;
		main_info.schema = "main";
		main_info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
		auto main_entry = make_shared_ptr<OracleSchemaEntry>(catalog, main_info);
		main_entry->oracle_name = ds;
		entries["main"] = std::move(main_entry);
	}
}

} // namespace duckdb
