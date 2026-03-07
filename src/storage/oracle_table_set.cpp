#include "storage/oracle_table_set.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_transaction.hpp"
#include "storage/oracle_schema_entry.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

OracleTableSet::OracleTableSet(OracleSchemaEntry &schema,
                                unique_ptr<OracleResultSlice> tables,
                                unique_ptr<OracleResultSlice> constraints)
    : OracleInSchemaSet(schema, !tables),
      table_result(std::move(tables)),
      constraint_result(std::move(constraints)) {
}

// ---------------------------------------------------------------------------
// Schema introspection queries
// ---------------------------------------------------------------------------

string OracleTableSet::GetColumnsQuery() {
	// Returns: owner, table_name, num_rows, column_name, data_type,
	//          data_length, data_precision, data_scale, nullable, column_id
	// all_tab_columns covers both tables and views.
	// Bind :owner and :table_name before executing.
	return R"(
SELECT c.owner, c.table_name, NVL(tbl.num_rows, 0) AS num_rows,
       c.column_name, c.data_type, c.data_length, c.data_precision, c.data_scale,
       c.nullable, c.column_id
FROM all_tab_columns c
LEFT JOIN all_tables tbl
  ON c.owner = tbl.owner AND c.table_name = tbl.table_name
WHERE c.owner      = :owner
  AND c.table_name = :table_name
ORDER BY c.column_id
)";
}

string OracleTableSet::GetConstraintsQuery() {
	// Returns: table_name, constraint_name, constraint_type, column_name, position
	// Bind :owner and :table_name before executing.
	return R"(
SELECT cc.table_name, cc.constraint_name, con.constraint_type,
       cc.column_name, cc.position
FROM all_cons_columns cc
JOIN all_constraints con
  ON cc.owner           = con.owner
 AND cc.constraint_name = con.constraint_name
 AND cc.table_name      = con.table_name
WHERE cc.owner      = :owner
  AND cc.table_name = :table_name
  AND con.constraint_type IN ('P','U')
ORDER BY cc.table_name, cc.constraint_name, cc.position
)";
}

string OracleTableSet::GetSchemaColumnsQuery() {
	// Bulk load: column info for ALL tables+views in a schema in one round-trip.
	// all_tab_columns covers both tables and views; LEFT JOIN all_tables for num_rows.
	// Bind :owner before executing.
	return R"(
SELECT c.owner, c.table_name, NVL(tbl.num_rows, 0) AS num_rows,
       c.column_name, c.data_type, c.data_length, c.data_precision, c.data_scale,
       c.nullable, c.column_id
FROM all_tab_columns c
LEFT JOIN all_tables tbl
  ON c.owner = tbl.owner AND c.table_name = tbl.table_name
WHERE c.owner = :owner
ORDER BY c.table_name, c.column_id
)";
}

string OracleTableSet::GetSchemaConstraintsQuery() {
	// Bulk load: all PK/UK constraints for all tables in a schema in one round-trip.
	// Bind :owner before executing.
	return R"(
SELECT cc.table_name, cc.constraint_name, con.constraint_type,
       cc.column_name, cc.position
FROM all_cons_columns cc
JOIN all_constraints con
  ON cc.owner           = con.owner
 AND cc.constraint_name = con.constraint_name
 AND cc.table_name      = con.table_name
WHERE cc.owner = :owner
  AND con.constraint_type IN ('P','U')
ORDER BY cc.table_name, cc.constraint_name, cc.position
)";
}

// ---------------------------------------------------------------------------
// Row parsing helpers
// ---------------------------------------------------------------------------

void OracleTableSet::AddColumn(OracleResult &result, idx_t row,
                                OracleTableInfo &table_info) {
	// col indices: 0=owner,1=table_name,2=num_rows,3=col_name,4=data_type,
	//              5=data_length,6=data_precision,7=data_scale,8=nullable,9=col_id
	OracleTypeData type_info;
	type_info.type_name = result.GetString(row, 4);
	type_info.data_length = result.IsNull(row, 5) ? 0 : result.GetInt64(row, 5);
	type_info.data_precision = result.IsNull(row, 6) ? -1 : result.GetInt64(row, 6);
	type_info.data_scale = result.IsNull(row, 7) ? -127 : result.GetInt64(row, 7);
	bool is_not_null = !result.IsNull(row, 8) && result.GetString(row, 8) == "N";

	auto col_name = result.GetString(row, 3);

	OracleType oracle_type;
	auto column_type = OracleUtils::TypeToLogicalType(type_info, oracle_type);
	table_info.oracle_types.push_back(std::move(oracle_type));
	table_info.oracle_names.push_back(col_name);

	ColumnDefinition column(col_name, std::move(column_type));
	auto &create_info = *table_info.create_info;
	if (is_not_null) {
		create_info.constraints.push_back(make_uniq<NotNullConstraint>(
		    LogicalIndex(create_info.columns.PhysicalColumnCount())));
	}
	create_info.columns.AddColumn(std::move(column));
}

// ---------------------------------------------------------------------------
// CreateEntries: build table entries from column + constraint results
// ---------------------------------------------------------------------------

void OracleTableSet::CreateEntries(OracleTransaction &transaction,
                                    OracleResult &col_result, OracleResult &con_result,
                                    idx_t col_start, idx_t col_end, idx_t con_start,
                                    idx_t con_end) {
	vector<unique_ptr<OracleTableInfo>> tables;
	unique_ptr<OracleTableInfo> info;

	// Process columns
	for (idx_t row = col_start; row < col_end; row++) {
		auto table_name = col_result.GetString(row, 1);
		if (!info || !StringUtil::CIEquals(info->GetTableName(), table_name)) {
			if (info) {
				tables.push_back(std::move(info));
			}
			int64_t num_rows = col_result.IsNull(row, 2) ? 0 : col_result.GetInt64(row, 2);
			info = make_uniq<OracleTableInfo>(schema, table_name);
			info->approx_num_rows = (idx_t)num_rows;
		}
		AddColumn(col_result, row, *info);
	}
	if (info) {
		tables.push_back(std::move(info));
	}

	// Process constraints
	// Group by table_name + constraint_name, then add to matching table info
	case_insensitive_map_t<case_insensitive_map_t<vector<string>>> pk_cols_by_table;
	case_insensitive_map_t<case_insensitive_map_t<string>> con_type_by_table;

	for (idx_t row = con_start; row < con_end; row++) {
		auto table_name = con_result.GetString(row, 0);
		auto constraint_name = con_result.GetString(row, 1);
		auto constraint_type = con_result.GetString(row, 2);
		auto column_name = con_result.GetString(row, 3);
		pk_cols_by_table[table_name][constraint_name].push_back(column_name);
		con_type_by_table[table_name][constraint_name] = constraint_type;
	}

	// Apply constraints to matching table entries
	for (auto &tbl_info : tables) {
		auto &tname = tbl_info->GetTableName();
		auto con_it = pk_cols_by_table.find(tname);
		if (con_it != pk_cols_by_table.end()) {
			for (auto &con_entry : con_it->second) {
				auto &cols = con_entry.second;
				bool is_pk = (con_type_by_table[tname][con_entry.first] == "P");
				tbl_info->create_info->constraints.push_back(
				    make_uniq<UniqueConstraint>(vector<string>(cols), is_pk));
			}
		}
		auto table_entry = make_shared_ptr<OracleTableEntry>(
		    static_cast<Catalog &>(catalog), static_cast<SchemaCatalogEntry &>(schema),
		    *tbl_info);
		CreateEntryInternal(std::move(table_entry));
	}
}

// ---------------------------------------------------------------------------
// LoadEntries
// ---------------------------------------------------------------------------

void OracleTableSet::LoadEntries(ClientContext &context, OracleTransaction &transaction) {
	// Bulk load: fetch all column and constraint info for the whole schema in two
	// round-trips, then build fully-populated entries via CreateEntries.
	// This avoids N per-table round-trips when the caller scans all tables.
	table_result.reset();
	constraint_result.reset();

	unordered_map<string, string> binds = {{"owner", StringUtil::Upper(schema.oracle_name)}};

	auto col_result = transaction.Query(GetSchemaColumnsQuery(), binds);
	if (!col_result || col_result->Count() == 0) {
		return;
	}

	// Constraints are optional — views have none, so an empty result is fine.
	auto con_result = transaction.Query(GetSchemaConstraintsQuery(), binds);
	OracleResult empty_con;
	auto &con_ref = con_result ? *con_result : empty_con;

	CreateEntries(transaction, *col_result, con_ref,
	              0, col_result->Count(),
	              0, con_ref.Count());
}

// ---------------------------------------------------------------------------
// GetTableInfo - single table
// ---------------------------------------------------------------------------

unique_ptr<OracleTableInfo> OracleTableSet::GetTableInfo(OracleTransaction &transaction,
                                                          OracleSchemaEntry &schema,
                                                          const string &table_name) {
	unordered_map<string, string> binds = {
	    {"owner",      StringUtil::Upper(schema.oracle_name)},
	    {"table_name", StringUtil::Upper(table_name)}};
	auto col_result = transaction.Query(GetColumnsQuery(), binds);
	if (!col_result || col_result->Count() == 0) {
		return nullptr;
	}
	auto con_result = transaction.Query(GetConstraintsQuery(), binds);

	auto table_info = make_uniq<OracleTableInfo>(schema, table_name);
	idx_t col_rows = col_result->Count();
	idx_t con_rows = con_result ? con_result->Count() : 0;

	for (idx_t row = 0; row < col_rows; row++) {
		AddColumn(*col_result, row, *table_info);
	}
	if (!col_result->IsNull(0, 2)) {
		table_info->approx_num_rows = (idx_t)col_result->GetInt64(0, 2);
	}
	// Add constraints
	case_insensitive_map_t<vector<string>> pk_cols;
	case_insensitive_map_t<string> con_types;
	OracleResult empty_con;
	auto &con_ref = con_result ? *con_result : empty_con;
	for (idx_t row = 0; row < con_rows; row++) {
		auto con_name = con_ref.GetString(row, 1);
		auto con_type = con_ref.GetString(row, 2);
		auto col_name = con_ref.GetString(row, 3);
		pk_cols[con_name].push_back(col_name);
		con_types[con_name] = con_type;
	}
	for (auto &entry : pk_cols) {
		bool is_pk = (con_types[entry.first] == "P");
		table_info->create_info->constraints.push_back(
		    make_uniq<UniqueConstraint>(vector<string>(entry.second), is_pk));
	}
	return table_info;
}

unique_ptr<OracleTableInfo> OracleTableSet::GetTableInfo(ClientContext &context,
                                                          OracleConnection &connection,
                                                          const string &schema_name,
                                                          const string &table_name) {
	unordered_map<string, string> binds = {
	    {"owner",      StringUtil::Upper(schema_name)},
	    {"table_name", StringUtil::Upper(table_name)}};
	auto col_result = connection.Query(context, GetColumnsQuery(), binds);
	if (!col_result || col_result->Count() == 0) {
		throw InvalidInputException("Table %s.%s does not exist or has no columns.",
		                             schema_name, table_name);
	}
	auto table_info = make_uniq<OracleTableInfo>(schema_name, table_name);
	idx_t col_rows = col_result->Count();
	for (idx_t row = 0; row < col_rows; row++) {
		AddColumn(*col_result, row, *table_info);
	}
	if (!col_result->IsNull(0, 2)) {
		table_info->approx_num_rows = (idx_t)col_result->GetInt64(0, 2);
	}
	return table_info;
}

// ---------------------------------------------------------------------------
// ReloadEntry
// ---------------------------------------------------------------------------

optional_ptr<CatalogEntry> OracleTableSet::ReloadEntry(OracleTransaction &transaction,
                                                         const string &table_name) {
	auto table_info = GetTableInfo(transaction, schema, table_name);
	if (!table_info) {
		return nullptr;
	}
	auto table_entry = make_shared_ptr<OracleTableEntry>(
	    static_cast<Catalog &>(catalog), static_cast<SchemaCatalogEntry &>(schema),
	    *table_info);
	return CreateEntryInternal(std::move(table_entry));
}

// ---------------------------------------------------------------------------
// CreateTable
// ---------------------------------------------------------------------------

static string GetOracleCreateTable(CreateTableInfo &info) {
	// Convert column types to Oracle SQL types
	std::stringstream ss;
	ss << "CREATE TABLE ";
	if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		// Oracle 23c+: IF NOT EXISTS; for older: check first
		// For simplicity we just try and catch errors upstream
	}
	if (!info.schema.empty()) {
		ss << OracleUtils::QuoteIdentifier(info.schema) << ".";
	}
	ss << OracleUtils::QuoteIdentifier(info.table) << " (";

	bool first = true;
	for (auto &col : info.columns.Logical()) {
		if (!first) ss << ", ";
		first = false;
		ss << OracleUtils::QuoteIdentifier(col.Name()) << " ";
		ss << OracleUtils::TypeToString(col.GetType());
	}
	// Constraints
	for (auto &constraint : info.constraints) {
		if (constraint->type == ConstraintType::UNIQUE) {
			auto &uc = constraint->Cast<UniqueConstraint>();
			if (!uc.columns.empty()) {
				ss << ", ";
				if (uc.is_primary_key) {
					ss << "PRIMARY KEY (";
				} else {
					ss << "UNIQUE (";
				}
				for (idx_t i = 0; i < uc.columns.size(); i++) {
					if (i > 0) ss << ", ";
					ss << OracleUtils::QuoteIdentifier(uc.columns[i]);
				}
				ss << ")";
			}
		}
	}
	ss << ")";
	return ss.str();
}

optional_ptr<CatalogEntry> OracleTableSet::CreateTable(OracleTransaction &transaction,
                                                         BoundCreateTableInfo &info) {
	auto create_sql = GetOracleCreateTable(info.Base());
	transaction.Query(create_sql);
	// Oracle DDL auto-commits; no explicit COMMIT needed here
	auto tbl_entry = make_shared_ptr<OracleTableEntry>(
	    static_cast<Catalog &>(catalog), static_cast<SchemaCatalogEntry &>(schema),
	    info.Base());
	return CreateEntry(transaction, std::move(tbl_entry));
}

// ---------------------------------------------------------------------------
// AlterTable
// ---------------------------------------------------------------------------

string OracleTableSet::GetAlterTablePrefix(ClientContext &context,
                                            OracleTransaction &transaction,
                                            const string &name) {
	string sql = "ALTER TABLE ";
	sql += OracleUtils::QuoteIdentifier(schema.oracle_name) + ".";
	sql += OracleUtils::QuoteIdentifier(name);
	return sql;
}

string OracleTableSet::GetAlterTablePrefix(const string &name,
                                            optional_ptr<CatalogEntry> entry) {
	string sql = "ALTER TABLE ";
	sql += OracleUtils::QuoteIdentifier(schema.oracle_name) + ".";
	sql += OracleUtils::QuoteIdentifier(entry ? entry->name : name);
	return sql;
}

void OracleTableSet::AlterTable(ClientContext &context, OracleTransaction &transaction,
                                  RenameTableInfo &info) {
	string sql = "RENAME " + OracleUtils::QuoteIdentifier(info.name) + " TO " +
	             OracleUtils::QuoteIdentifier(info.new_table_name);
	transaction.Query(sql);
}

void OracleTableSet::AlterTable(ClientContext &context, OracleTransaction &transaction,
                                  RenameColumnInfo &info) {
	string sql = GetAlterTablePrefix(context, transaction, info.name);
	sql += " RENAME COLUMN " + OracleUtils::QuoteIdentifier(info.old_name) + " TO " +
	       OracleUtils::QuoteIdentifier(info.new_name);
	transaction.Query(sql);
}

void OracleTableSet::AlterTable(ClientContext &context, OracleTransaction &transaction,
                                  AddColumnInfo &info) {
	string sql = GetAlterTablePrefix(context, transaction, info.name);
	sql += " ADD " + OracleUtils::QuoteIdentifier(info.new_column.Name()) + " " +
	       OracleUtils::TypeToString(info.new_column.Type());
	transaction.Query(sql);
}

void OracleTableSet::AlterTable(ClientContext &context, OracleTransaction &transaction,
                                  RemoveColumnInfo &info) {
	string sql = GetAlterTablePrefix(context, transaction, info.name);
	sql += " DROP COLUMN " + OracleUtils::QuoteIdentifier(info.removed_column);
	transaction.Query(sql);
}

void OracleTableSet::AlterTable(ClientContext &context, OracleTransaction &transaction,
                                  AlterTableInfo &alter) {
	switch (alter.alter_table_type) {
	case AlterTableType::RENAME_TABLE:
		AlterTable(context, transaction, alter.Cast<RenameTableInfo>());
		break;
	case AlterTableType::RENAME_COLUMN:
		AlterTable(context, transaction, alter.Cast<RenameColumnInfo>());
		break;
	case AlterTableType::ADD_COLUMN:
		AlterTable(context, transaction, alter.Cast<AddColumnInfo>());
		break;
	case AlterTableType::REMOVE_COLUMN:
		AlterTable(context, transaction, alter.Cast<RemoveColumnInfo>());
		break;
	default:
		throw BinderException(
		    "Unsupported ALTER TABLE type for Oracle - only RENAME TABLE, "
		    "RENAME COLUMN, ADD COLUMN and DROP COLUMN are supported");
	}
	ClearEntries();
}

} // namespace duckdb
