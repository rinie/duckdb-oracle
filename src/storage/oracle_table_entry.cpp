#include "storage/oracle_table_entry.hpp"
#include "storage/oracle_schema_entry.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_transaction.hpp"
#include "oracle_scanner.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"

namespace duckdb {

// ---------------------------------------------------------------------------
// OracleTableInfo
// ---------------------------------------------------------------------------

OracleTableInfo::OracleTableInfo(OracleSchemaEntry &schema, const string &table_name)
    : schema(&schema), schema_name(schema.name) {
	create_info = make_uniq<CreateTableInfo>();
	create_info->schema = schema.name;
	create_info->table = table_name;
}

OracleTableInfo::OracleTableInfo(const string &schema_name, const string &table_name)
    : schema(nullptr), schema_name(schema_name) {
	create_info = make_uniq<CreateTableInfo>();
	create_info->schema = schema_name;
	create_info->table = table_name;
}

// ---------------------------------------------------------------------------
// OracleTableEntry
// ---------------------------------------------------------------------------

OracleTableEntry::OracleTableEntry(Catalog &catalog, SchemaCatalogEntry &schema,
                                    OracleTableInfo &info)
    : TableCatalogEntry(catalog, schema, *info.create_info),
      oracle_schema_name(schema.Cast<OracleSchemaEntry>().oracle_name),
      oracle_names(info.oracle_names), oracle_types(info.oracle_types),
      approx_num_rows(info.approx_num_rows) {
}

OracleTableEntry::OracleTableEntry(Catalog &catalog, SchemaCatalogEntry &schema,
                                    CreateTableInfo &info)
    : TableCatalogEntry(catalog, schema, info),
      oracle_schema_name(schema.Cast<OracleSchemaEntry>().oracle_name) {
}

unique_ptr<BaseStatistics> OracleTableEntry::GetStatistics(ClientContext &context,
                                                            column_t column_id) {
	return nullptr;
}

TableFunction OracleTableEntry::GetScanFunction(ClientContext &context,
                                                  unique_ptr<FunctionData> &bind_data) {
	OracleScanFunction fun;
	auto result = make_uniq<OracleBindData>();
	auto &catalog = this->catalog.Cast<OracleCatalog>();

	result->dsn = catalog.connection_string;
	result->attach_path = catalog.attach_path;
	result->schema_name = oracle_schema_name;
	result->table_name = this->name;
	result->oracle_types = oracle_types;
	result->approx_num_rows = approx_num_rows;
	result->requires_materialization = false;
	result->can_use_main_thread = true;
	result->max_threads = 1;

	for (auto &col : columns.Logical()) {
		result->names.push_back(col.GetName());
		result->types.push_back(col.GetType());
	}
	result->oracle_names = oracle_names;
	result->SetCatalog(catalog);
	result->SetTable(*this);

	bind_data = std::move(result);
	return fun;
}

TableStorageInfo OracleTableEntry::GetStorageInfo(ClientContext &context) {
	TableStorageInfo result;
	result.cardinality = approx_num_rows;
	return result;
}

void OracleTableEntry::BindUpdateConstraints(Binder &binder, LogicalGet &get,
                                              LogicalProjection &proj,
                                              LogicalUpdate &update,
                                              ClientContext &context) {
}

string OracleTableEntry::GetCopyFormat(ClientContext &context) const {
	return ""; // Not applicable for Oracle
}

PhysicalIndex OracleTableEntry::GetColumnIndex(const string &name,
                                                bool silence_error) const {
	// Search oracle_names (case-insensitive)
	for (idx_t i = 0; i < oracle_names.size(); i++) {
		if (StringUtil::CIEquals(oracle_names[i], name)) {
			return PhysicalIndex(i);
		}
	}
	if (!silence_error) {
		throw InternalException("Oracle table entry: column \"%s\" not found", name);
	}
	return PhysicalIndex(DConstants::INVALID_INDEX);
}

} // namespace duckdb
