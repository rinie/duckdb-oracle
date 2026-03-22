#include "storage/oracle_index_set.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_schema_entry.hpp"
#include "storage/oracle_transaction.hpp"
#include "storage/oracle_index_entry.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "oracle_utils.hpp"

namespace duckdb {

OracleIndexSet::OracleIndexSet(OracleSchemaEntry &schema)
    : OracleInSchemaSet(schema, false) {
}

// Oracle data dictionary value for unique indexes.
static constexpr const char *kOracleUnique = "UNIQUE";

void OracleIndexSet::PopulateFromResult(OracleResult &result, idx_t start, idx_t end) {
	// col layout: 0=index_name, 1=table_name, 2=uniqueness, 3=column_name, 4=column_position
	case_insensitive_map_t<unique_ptr<CreateIndexInfo>> index_map;
	for (idx_t row = start; row < end; row++) {
		auto idx_name   = result.GetString(row, 0);
		auto tbl_name   = result.GetString(row, 1);
		auto uniqueness = result.GetString(row, 2);

		if (index_map.find(idx_name) == index_map.end()) {
			auto info = make_uniq<CreateIndexInfo>();
			info->index_name = idx_name;
			info->table = tbl_name;
			info->constraint_type = (uniqueness == kOracleUnique)
			                             ? IndexConstraintType::UNIQUE
			                             : IndexConstraintType::NONE;
			index_map[idx_name] = std::move(info);
		}
	}
	for (auto &entry : index_map) {
		auto idx_entry = make_shared_ptr<OracleIndexEntry>(
		    static_cast<Catalog &>(catalog), static_cast<SchemaCatalogEntry &>(schema),
		    *entry.second);
		CreateEntryInternal(std::move(idx_entry));
	}
}

void OracleIndexSet::LoadEntries(ClientContext &context, OracleTransaction &transaction) {
	auto level = schema.ParentCatalog().Cast<OracleCatalog>().GetPrivilegeLevel();

	// Choose the index / ind_columns views based on privilege level.
	// USER_* has no :owner bind and covers only the connected user's own indexes.
	// ALL_* / DBA_* need an explicit owner literal.
	string query;
	if (level == OraclePrivilegeLevel::USER ||
	    (level == OraclePrivilegeLevel::ALL && schema.IsCurrentUserSchema())) {
		query = R"(
SELECT LOWER(i.index_name), LOWER(i.table_name), i.uniqueness,
       LOWER(ic.column_name), ic.column_position
FROM user_indexes i
JOIN user_ind_columns ic
  ON i.index_name = ic.index_name
 AND i.table_name = ic.table_name
ORDER BY i.index_name, ic.column_position
)";
	} else {
		const char *idx_view = (level == OraclePrivilegeLevel::DBA) ? "dba_indexes"     : "all_indexes";
		const char *ic_view  = (level == OraclePrivilegeLevel::DBA) ? "dba_ind_columns" : "all_ind_columns";
		query = StringUtil::Format(R"(
SELECT LOWER(i.index_name), LOWER(i.table_name), i.uniqueness,
       LOWER(ic.column_name), ic.column_position
FROM %s i
JOIN %s ic ON i.owner = ic.index_owner
  AND i.index_name = ic.index_name
  AND i.table_name = ic.table_name
WHERE i.owner = %s
ORDER BY i.index_name, ic.column_position
)",
		    idx_view, ic_view, OracleUtils::WriteLiteral(StringUtil::Upper(schema.oracle_name)));
	}

	auto result = transaction.Query(query);
	if (!result || result->Count() == 0) {
		return;
	}
	PopulateFromResult(*result, 0, result->Count());
}

} // namespace duckdb
