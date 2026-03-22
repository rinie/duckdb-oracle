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
    : OracleInSchemaSet(schema, false), // always start unloaded; lazy stubs via LoadEntries
      table_result(std::move(tables)),
      constraint_result(std::move(constraints)) {
}

// ---------------------------------------------------------------------------
// Schema introspection queries
// ---------------------------------------------------------------------------

string OracleTableSet::GetColumnsQuery() {
	// Returns: owner(0), table_name(1), num_rows(2), column_name(3), data_type(4),
	//          data_length(5), data_precision(6), data_scale(7), nullable(8),
	//          table_type(9), column_id(10)
	// Bind :owner and :table_name before executing.
	return R"(
SELECT LOWER(c.owner), LOWER(c.table_name), NVL(t.num_rows, 0) AS num_rows,
       LOWER(c.column_name), c.data_type, c.data_length, c.data_precision, c.data_scale,
       c.nullable, 'BASE TABLE' AS table_type, c.column_id
FROM all_tab_columns c
JOIN all_tables t ON t.owner = c.owner AND t.table_name = c.table_name
WHERE c.owner      = :owner
  AND c.table_name = :table_name
UNION ALL
SELECT LOWER(c.owner), LOWER(c.table_name), 0 AS num_rows,
       LOWER(c.column_name), c.data_type, c.data_length, c.data_precision, c.data_scale,
       c.nullable, 'VIEW' AS table_type, c.column_id
FROM all_tab_columns c
JOIN all_views v ON v.owner = c.owner AND v.view_name = c.table_name
WHERE c.owner      = :owner
  AND c.table_name = :table_name
ORDER BY 11
)";
}

string OracleTableSet::GetConstraintsQuery() {
	// Returns: table_name(0), constraint_name(1), constraint_type(2), column_name(3), position(4)
	// Bind :owner and :table_name before executing.
	return R"(
SELECT LOWER(cc.table_name), cc.constraint_name, con.constraint_type,
       LOWER(cc.column_name), cc.position
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
	// Bulk load: all tables+views in a schema (UNION ALL for VIEW tracking).
	// Bind :owner before executing.
	return R"(
SELECT LOWER(c.owner), LOWER(c.table_name), NVL(t.num_rows, 0) AS num_rows,
       LOWER(c.column_name), c.data_type, c.data_length, c.data_precision, c.data_scale,
       c.nullable, 'BASE TABLE' AS table_type, c.column_id
FROM all_tab_columns c
JOIN all_tables t ON t.owner = c.owner AND t.table_name = c.table_name
WHERE c.owner = :owner
UNION ALL
SELECT LOWER(c.owner), LOWER(c.table_name), 0 AS num_rows,
       LOWER(c.column_name), c.data_type, c.data_length, c.data_precision, c.data_scale,
       c.nullable, 'VIEW' AS table_type, c.column_id
FROM all_tab_columns c
JOIN all_views v ON v.owner = c.owner AND v.view_name = c.table_name
WHERE c.owner = :owner
ORDER BY 2, 11
)";
}

string OracleTableSet::GetSchemaConstraintsQuery() {
	// Bulk load: all PK/UK constraints for all tables in a schema.
	// Bind :owner before executing.
	return R"(
SELECT LOWER(cc.table_name), cc.constraint_name, con.constraint_type,
       LOWER(cc.column_name), cc.position
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
// USER_* view variants (no privilege check, faster for the connected user)
// ---------------------------------------------------------------------------

string OracleTableSet::GetUserColumnsQuery() {
	// Same column layout as GetColumnsQuery(). Bind :table_name only.
	return R"(
SELECT LOWER(USER) AS owner, LOWER(c.table_name), 0 AS num_rows,
       LOWER(c.column_name), c.data_type, c.data_length, c.data_precision, c.data_scale,
       c.nullable, 'BASE TABLE' AS table_type, c.column_id
FROM user_tab_columns c
JOIN user_tables t ON t.table_name = c.table_name
WHERE c.table_name = :table_name
UNION ALL
SELECT LOWER(USER) AS owner, LOWER(c.table_name), 0 AS num_rows,
       LOWER(c.column_name), c.data_type, c.data_length, c.data_precision, c.data_scale,
       c.nullable, 'VIEW' AS table_type, c.column_id
FROM user_tab_columns c
JOIN user_views v ON v.view_name = c.table_name
WHERE c.table_name = :table_name
ORDER BY 11
)";
}

string OracleTableSet::GetUserConstraintsQuery() {
	// Bind :table_name only.
	return R"(
SELECT LOWER(cc.table_name), cc.constraint_name, con.constraint_type,
       LOWER(cc.column_name), cc.position
FROM user_cons_columns cc
JOIN user_constraints con
  ON cc.constraint_name = con.constraint_name
 AND cc.table_name      = con.table_name
WHERE cc.table_name = :table_name
  AND con.constraint_type IN ('P','U')
ORDER BY cc.table_name, cc.constraint_name, cc.position
)";
}

string OracleTableSet::GetUserSchemaColumnsQuery() {
	// No bind params needed — USER implicitly scopes to the connected user.
	return R"(
SELECT LOWER(USER) AS owner, LOWER(c.table_name), 0 AS num_rows,
       LOWER(c.column_name), c.data_type, c.data_length, c.data_precision, c.data_scale,
       c.nullable, 'BASE TABLE' AS table_type, c.column_id
FROM user_tab_columns c
JOIN user_tables t ON t.table_name = c.table_name
UNION ALL
SELECT LOWER(USER) AS owner, LOWER(c.table_name), 0 AS num_rows,
       LOWER(c.column_name), c.data_type, c.data_length, c.data_precision, c.data_scale,
       c.nullable, 'VIEW' AS table_type, c.column_id
FROM user_tab_columns c
JOIN user_views v ON v.view_name = c.table_name
ORDER BY 2, 11
)";
}

string OracleTableSet::GetUserSchemaConstraintsQuery() {
	// No bind params needed.
	return R"(
SELECT LOWER(cc.table_name), cc.constraint_name, con.constraint_type,
       LOWER(cc.column_name), cc.position
FROM user_cons_columns cc
JOIN user_constraints con
  ON cc.constraint_name = con.constraint_name
 AND cc.table_name      = con.table_name
WHERE con.constraint_type IN ('P','U')
ORDER BY cc.table_name, cc.constraint_name, cc.position
)";
}

// ---------------------------------------------------------------------------
// Row parsing helpers
// ---------------------------------------------------------------------------

void OracleTableSet::AddColumn(OracleResult &result, idx_t row,
                                OracleTableInfo &table_info) {
	// col indices: 0=owner, 1=table_name, 2=num_rows, 3=col_name, 4=data_type,
	//              5=data_length, 6=data_precision, 7=data_scale, 8=nullable,
	//              9=table_type, 10=col_id
	OracleTypeData type_info;
	type_info.type_name = result.GetString(row, 4);
	type_info.data_length = result.IsNull(row, 5) ? 0 : result.GetInt64(row, 5);
	type_info.data_precision = result.IsNull(row, 6) ? -1 : result.GetInt64(row, 6);
	type_info.data_scale = result.IsNull(row, 7) ? -127 : result.GetInt64(row, 7);
	bool is_not_null = !result.IsNull(row, 8) && result.GetString(row, 8) == "N";

	// col 9: table_type — mark the entry as a view when Oracle reports 'VIEW'
	if (!result.IsNull(row, 9)) {
		table_info.is_view = (result.GetString(row, 9) == "VIEW");
	}

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

	// Apply constraints and register entries
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
		if (tbl_info->is_view) {
			table_entry->type = CatalogType::VIEW_ENTRY;
		}
		CreateEntryInternal(std::move(table_entry));
	}
}

// ---------------------------------------------------------------------------
// LoadEntries — lazy: cheap names-only query, stubs upgraded on first access
// ---------------------------------------------------------------------------

void OracleTableSet::LoadEntries(ClientContext &context, OracleTransaction &transaction) {
	// Discard any pre-loaded slices — lazy loading supersedes that path.
	table_result.reset();
	constraint_result.reset();

	bool is_current_user = schema.IsCurrentUserSchema();

	string q;
	unordered_map<string, string> binds;

	if (is_current_user) {
		// USER_* views skip privilege checks — much faster for the connected user's schema.
		q = "SELECT LOWER(table_name), 'BASE TABLE' FROM user_tables "
		    "UNION ALL SELECT LOWER(view_name), 'VIEW' FROM user_views "
		    "ORDER BY 1";
	} else {
		binds = {{"owner", StringUtil::Upper(schema.oracle_name)}};
		q = "SELECT LOWER(table_name), 'BASE TABLE' FROM all_tables WHERE owner = :owner "
		    "UNION ALL SELECT LOWER(view_name), 'VIEW' FROM all_views WHERE owner = :owner "
		    "ORDER BY 1";
	}

	auto result = transaction.Query(q, binds);
	if (!result) {
		return;
	}

	for (idx_t row = 0; row < result->Count(); row++) {
		auto table_name = result->GetString(row, 0);
		bool is_view = (result->GetString(row, 1) == "VIEW");

		// Stub entry: name + type only, no columns.
		// GetEntry transparently upgrades the stub via ReloadEntry on first access.
		auto table_info = make_uniq<OracleTableInfo>(schema, table_name);
		table_info->is_view = is_view;
		auto stub = make_shared_ptr<OracleTableEntry>(
		    static_cast<Catalog &>(catalog),
		    static_cast<SchemaCatalogEntry &>(schema),
		    *table_info);
		if (is_view) {
			stub->type = CatalogType::VIEW_ENTRY;
		}
		entries[table_name] = std::move(stub);
		MarkAsStub(table_name);
	}
}

// ---------------------------------------------------------------------------
// GetTableInfo - single table (used by ReloadEntry and external scan path)
// ---------------------------------------------------------------------------

unique_ptr<OracleTableInfo> OracleTableSet::GetTableInfo(OracleTransaction &transaction,
                                                          OracleSchemaEntry &schema,
                                                          const string &table_name) {
	bool is_current_user = schema.IsCurrentUserSchema();

	// ALL_* needs :owner + :table_name; USER_* needs only :table_name.
	const string upper_table = StringUtil::Upper(table_name);
	unordered_map<string, string> all_binds = {
	    {"owner",      StringUtil::Upper(schema.oracle_name)},
	    {"table_name", upper_table}};
	unordered_map<string, string> user_binds = {{"table_name", upper_table}};

	const string &cols_query = is_current_user ? GetUserColumnsQuery() : GetColumnsQuery();
	const string &cons_query = is_current_user ? GetUserConstraintsQuery() : GetConstraintsQuery();
	const auto &query_binds  = is_current_user ? user_binds : all_binds;

	auto col_result = transaction.Query(cols_query, query_binds);
	if (!col_result || col_result->Count() == 0) {
		return nullptr;
	}
	auto con_result = transaction.Query(cons_query, query_binds);

	auto table_info = make_uniq<OracleTableInfo>(schema, table_name);
	idx_t col_rows = col_result->Count();
	idx_t con_rows = con_result ? con_result->Count() : 0;

	// Set num_rows from first row col 2
	if (!col_result->IsNull(0, 2)) {
		table_info->approx_num_rows = (idx_t)col_result->GetInt64(0, 2);
	}
	for (idx_t row = 0; row < col_rows; row++) {
		AddColumn(*col_result, row, *table_info); // also sets is_view via col 9
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
	// External path (e.g. oracle_scanner.cpp) — always uses ALL_* queries.
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
	if (!col_result->IsNull(0, 2)) {
		table_info->approx_num_rows = (idx_t)col_result->GetInt64(0, 2);
	}
	for (idx_t row = 0; row < col_rows; row++) {
		AddColumn(*col_result, row, *table_info);
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
	if (table_info->is_view) {
		table_entry->type = CatalogType::VIEW_ENTRY;
	}
	return CreateEntryInternal(std::move(table_entry));
}

// ---------------------------------------------------------------------------
// CreateTable
// ---------------------------------------------------------------------------

static string GetOracleCreateTable(CreateTableInfo &info) {
	std::stringstream ss;
	ss << "CREATE TABLE ";
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
