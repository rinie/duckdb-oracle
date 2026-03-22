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
	const string &internal_sql = OracleSchemaEntry::InternalOwnersSQL();
	string query;
	if (!schema_to_load.empty()) {
		// Load only the requested schema
		query = StringUtil::Format(
		    "SELECT DISTINCT LOWER(owner) FROM all_tables WHERE owner = %s "
		    "UNION SELECT DISTINCT LOWER(owner) FROM all_views WHERE owner = %s",
		    OracleUtils::WriteLiteral(StringUtil::Upper(schema_to_load)),
		    OracleUtils::WriteLiteral(StringUtil::Upper(schema_to_load)));
	} else {
		// Load all visible schemas/owners, filtering known internal ones in SQL
		query = StringUtil::Format(
		    "SELECT DISTINCT LOWER(owner) FROM all_tables WHERE owner NOT IN (%s) "
		    "UNION SELECT DISTINCT LOWER(owner) FROM all_views WHERE owner NOT IN (%s) "
		    "ORDER BY 1",
		    internal_sql, internal_sql);
	}

	auto result = transaction.Query(query);
	if (!result) {
		return;
	}

	for (idx_t row = 0; row < result->Count(); row++) {
		auto owner = result->GetString(row, 0); // already lowercase from LOWER()
		// Client-side safety net for non-standard internal schemas
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

} // namespace duckdb
