#include "duckdb/main/client_context.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "storage/oracle_index_entry.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"

namespace duckdb {

OracleIndexEntry::OracleIndexEntry(Catalog &catalog, SchemaCatalogEntry &schema,
                                    CreateIndexInfo &info)
    : IndexCatalogEntry(catalog, schema, info), table_name(info.table.GetIdentifierName()) {
}

Identifier OracleIndexEntry::GetSchemaName() const {
	return schema.name;
}

Identifier OracleIndexEntry::GetTableName() const {
	return Identifier(table_name);
}

} // namespace duckdb
