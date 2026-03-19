#include "storage/oracle_schema_set.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_transaction.hpp"
#include "storage/oracle_schema_entry.hpp"
#include "storage/oracle_table_set.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"

namespace duckdb {

// ---------------------------------------------------------------------------
// Global bulk-load query helpers (Optimization 4)
// ---------------------------------------------------------------------------

// Returns column query for ALL visible schemas at once (sorted by owner, table, column_id)
static string GetGlobalColumnsQuery() {
	return StringUtil::Format(R"(
SELECT c.owner, c.table_name, 0 AS num_rows,
       c.column_name, c.data_type, c.data_length, c.data_precision, c.data_scale,
       c.nullable, c.column_id
FROM all_tab_columns c
WHERE c.owner NOT IN (%s)
ORDER BY c.owner, c.table_name, c.column_id
)", OracleSchemaEntry::InternalOwnersSQL());
}

static string GetGlobalConstraintsQuery() {
	return StringUtil::Format(R"(
SELECT cc.owner, cc.table_name, cc.constraint_name, con.constraint_type,
       cc.column_name, cc.position
FROM all_cons_columns cc
JOIN all_constraints con
  ON cc.owner           = con.owner
 AND cc.constraint_name = con.constraint_name
 AND cc.table_name      = con.table_name
WHERE cc.owner NOT IN (%s)
  AND con.constraint_type IN ('P','U')
ORDER BY cc.owner, cc.table_name, cc.constraint_name, cc.position
)", OracleSchemaEntry::InternalOwnersSQL());
}

static string GetGlobalIndexesQuery() {
	return StringUtil::Format(R"(
SELECT i.owner, i.index_name, i.table_name, i.uniqueness,
       ic.column_name, ic.column_position
FROM all_indexes i
JOIN all_ind_columns ic
  ON i.owner      = ic.index_owner
 AND i.index_name = ic.index_name
 AND i.table_name = ic.table_name
WHERE i.owner NOT IN (%s)
ORDER BY i.owner, i.index_name, ic.column_position
)", OracleSchemaEntry::InternalOwnersSQL());
}

// Find contiguous row ranges per owner in a result set sorted by owner (col 0).
// Oracle always returns owner names in uppercase, so plain string comparison is safe.
static case_insensitive_map_t<pair<idx_t, idx_t>>
FindOwnerRanges(const unique_ptr<OracleResult> &res) {
	case_insensitive_map_t<pair<idx_t, idx_t>> ranges;
	if (!res) {
		return ranges;
	}
	string cur_owner;
	idx_t cur_start = 0;
	for (idx_t r = 0; r < res->Count(); r++) {
		const auto &owner = res->GetString(r, 0); // owner is always uppercase from Oracle
		if (owner != cur_owner) {
			if (!cur_owner.empty()) {
				ranges[cur_owner] = {cur_start, r};
			}
			cur_owner = owner;
			cur_start = r;
		}
	}
	if (!cur_owner.empty()) {
		ranges[cur_owner] = {cur_start, res->Count()};
	}
	return ranges;
}

// Copy rows [start, end) from src into a new OracleResultSlice.
// If strip_owner_col is true, col 0 (owner) is omitted from each row.
static unique_ptr<OracleResultSlice> BuildSlice(OracleResult &src, idx_t start, idx_t end,
                                                  bool strip_owner_col) {
	auto slice = make_uniq<OracleResultSlice>();
	slice->result.rows.reserve(end - start);
	for (idx_t r = start; r < end; r++) {
		if (strip_owner_col) {
			const auto &src_row = src.rows[r];
			slice->result.rows.emplace_back(src_row.begin() + 1, src_row.end());
		} else {
			slice->result.rows.push_back(std::move(src.rows[r]));
		}
	}
	slice->start = 0;
	slice->end   = slice->result.Count();
	return slice;
}

// ---------------------------------------------------------------------------
// OracleSchemaSet
// ---------------------------------------------------------------------------

OracleSchemaSet::OracleSchemaSet(OracleCatalog &catalog, const string &schema_to_load)
    : OracleCatalogSet(catalog), schema_to_load(schema_to_load) {
}

void OracleSchemaSet::LoadEntries(ClientContext &context, OracleTransaction &transaction) {
	const string &internal_sql = OracleSchemaEntry::InternalOwnersSQL();

	string query;
	if (!schema_to_load.empty()) {
		// Load only the requested schema
		query = StringUtil::Format(
		    "SELECT DISTINCT owner FROM all_tables WHERE owner = %s "
		    "UNION SELECT DISTINCT owner FROM all_views WHERE owner = %s",
		    OracleUtils::WriteLiteral(StringUtil::Upper(schema_to_load)),
		    OracleUtils::WriteLiteral(StringUtil::Upper(schema_to_load)));
	} else {
		// Load all visible schemas/owners, filtering internal ones in SQL
		query = StringUtil::Format(
		    "SELECT DISTINCT owner FROM all_tables WHERE owner NOT IN (%s) "
		    "UNION SELECT DISTINCT owner FROM all_views WHERE owner NOT IN (%s) "
		    "ORDER BY 1",
		    internal_sql, internal_sql);
	}

	auto result = transaction.Query(query);

	// Collect valid owners (client-side safety net for non-standard internal schemas).
	// Wrapped in if(result) so a query failure still lets "main" alias be registered below.
	vector<string> owners;
	if (result) {
		for (idx_t row = 0; row < result->Count(); row++) {
			auto owner = result->GetString(row, 0);
			if (OracleSchemaEntry::SchemaIsInternal(owner)) {
				continue;
			}
			owners.push_back(owner);
		}
	}

	if (!owners.empty()) {
		if (schema_to_load.empty() && owners.size() > 1) {
			// ── Multi-schema bulk load ─────────────────────────────────────────
			// Fire 3 global queries (no per-schema WHERE) then partition by owner.
			auto col_result = transaction.Query(GetGlobalColumnsQuery());
			auto con_result = transaction.Query(GetGlobalConstraintsQuery());
			auto idx_result = transaction.Query(GetGlobalIndexesQuery());

			auto col_ranges = FindOwnerRanges(col_result);
			auto con_ranges = FindOwnerRanges(con_result);
			auto idx_ranges = FindOwnerRanges(idx_result);

			for (auto &owner : owners) {
				// Column slice: keep owner column (col 0) — OracleTableSet expects it.
				// Rows are moved out of col_result (not copied) since it is discarded afterwards.
				unique_ptr<OracleResultSlice> col_slice;
				auto col_it = col_ranges.find(owner);
				if (col_it != col_ranges.end()) {
					col_slice = BuildSlice(*col_result, col_it->second.first,
					                       col_it->second.second, /*strip_owner_col=*/false);
				}

				// Constraint slice: strip owner (col 0).
				// CreateEntries expects: table_name(0), constraint_name(1), type(2), col(3), pos(4)
				// Global query:          owner(0), table_name(1), ...
				unique_ptr<OracleResultSlice> con_slice;
				auto con_it = con_ranges.find(owner);
				if (con_it != con_ranges.end()) {
					con_slice = BuildSlice(*con_result, con_it->second.first,
					                       con_it->second.second, /*strip_owner_col=*/true);
				}

				// Index slice: strip owner (col 0).
				// PopulateFromResult expects: index_name(0), table_name(1), uniqueness(2), ...
				// Global query:              owner(0), index_name(1), ...
				unique_ptr<OracleResultSlice> idx_slice;
				auto idx_it = idx_ranges.find(owner);
				if (idx_it != idx_ranges.end()) {
					idx_slice = BuildSlice(*idx_result, idx_it->second.first,
					                       idx_it->second.second, /*strip_owner_col=*/true);
				}

				CreateSchemaInfo info;
				info.schema = owner;
				info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
				auto entry = make_shared_ptr<OracleSchemaEntry>(catalog, info,
				                                                  std::move(col_slice),
				                                                  std::move(con_slice),
				                                                  std::move(idx_slice));
				entries[owner] = std::move(entry);
			}
		} else {
			// ── Single-schema path (or only 1 schema visible) ─────────────────
			for (auto &owner : owners) {
				CreateSchemaInfo info;
				info.schema = owner;
				info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
				auto entry = make_shared_ptr<OracleSchemaEntry>(catalog, info);
				entries[owner] = std::move(entry);
			}
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
