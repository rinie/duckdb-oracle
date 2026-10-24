#include "duckdb/main/client_context.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "storage/oracle_view_entry.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"

namespace duckdb {

CreateViewInfo OracleViewEntry::MakeViewInfo(const string &view_name) {
	CreateViewInfo info;
	info.SetViewName(Identifier(view_name));
	// query = nullptr  — Oracle view SQL is not exposed to DuckDB
	// sql   = ""       — same reason
	// aliases / column_types left empty
	return info;
}

// Delegating constructor: create a local CreateViewInfo, then forward to
// ViewCatalogEntry.  We can't construct the base with a temporary directly
// because ViewCatalogEntry takes CreateViewInfo by non-const reference.
OracleViewEntry::OracleViewEntry(Catalog &catalog, SchemaCatalogEntry &schema,
                                  CreateViewInfo info_storage)
    : ViewCatalogEntry(catalog, schema, info_storage) {
}

OracleViewEntry::OracleViewEntry(Catalog &catalog, SchemaCatalogEntry &schema,
                                  const string &view_name)
    : OracleViewEntry(catalog, schema, MakeViewInfo(view_name)) {
}

} // namespace duckdb
