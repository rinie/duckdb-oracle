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
    : schema(&schema), schema_name(schema.name.GetIdentifierName()), oracle_schema_name(schema.oracle_name) {
	create_info = make_uniq<CreateTableInfo>(
	    QualifiedName(Identifier(), Identifier(schema_name), Identifier(table_name)));
}

OracleTableInfo::OracleTableInfo(const string &schema_name, const string &table_name)
    : schema(nullptr), schema_name(schema_name), oracle_schema_name(schema_name) {
	create_info = make_uniq<CreateTableInfo>(
	    QualifiedName(Identifier(), Identifier(schema_name), Identifier(table_name)));
}

// ---------------------------------------------------------------------------
// OracleTableEntry
// ---------------------------------------------------------------------------

OracleTableEntry::OracleTableEntry(Catalog &catalog, SchemaCatalogEntry &schema,
                                    OracleTableInfo &info)
    : TableCatalogEntry(catalog, schema, *info.create_info), columns(std::move(info.create_info->columns)),
      oracle_schema_name(schema.Cast<OracleSchemaEntry>().oracle_name),
      is_view(info.is_view),
      oracle_names(info.oracle_names), oracle_types(info.oracle_types),
      approx_num_rows(info.approx_num_rows) {
}

OracleTableEntry::OracleTableEntry(Catalog &catalog, SchemaCatalogEntry &schema,
                                    CreateTableInfo &info)
    : TableCatalogEntry(catalog, schema, info), columns(std::move(info.columns)),
      oracle_schema_name(schema.Cast<OracleSchemaEntry>().oracle_name) {
}

const ColumnList &OracleTableEntry::GetColumns() const {
	return columns;
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
	result->table_name = this->name.GetIdentifierName();
	result->oracle_types = oracle_types;
	// an entry created from DDL alone has no Oracle types: use defaults rather than reading past the end
	result->oracle_types.resize(columns.LogicalColumnCount());
	result->approx_num_rows = approx_num_rows;
	result->requires_materialization = false;
	result->can_use_main_thread = true;
	result->max_threads = 1;

	for (auto &col : columns.Logical()) {
		result->names.push_back(col.GetName().GetIdentifierName());
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
