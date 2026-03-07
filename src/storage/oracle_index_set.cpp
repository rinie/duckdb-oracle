#include "storage/oracle_index_set.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_schema_entry.hpp"
#include "storage/oracle_transaction.hpp"
#include "storage/oracle_index_entry.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"

namespace duckdb {

OracleIndexSet::OracleIndexSet(OracleSchemaEntry &schema,
                                unique_ptr<OracleResultSlice> index_result)
    : OracleInSchemaSet(schema, !index_result),
      index_result(std::move(index_result)) {
}

void OracleIndexSet::LoadEntries(ClientContext &context, OracleTransaction &transaction) {
	// Query index metadata from Oracle data dictionary
	string query = StringUtil::Format(R"(
SELECT i.index_name, i.table_name, i.uniqueness,
       ic.column_name, ic.column_position
FROM all_indexes i
JOIN all_ind_columns ic ON i.owner = ic.index_owner
  AND i.index_name = ic.index_name
  AND i.table_name = ic.table_name
WHERE i.owner = %s
ORDER BY i.index_name, ic.column_position
)",
	    OracleUtils::WriteLiteral(StringUtil::Upper(schema.oracle_name)));

	auto result = transaction.Query(query);
	if (!result || result->Count() == 0) {
		return;
	}

	// Group by index_name
	case_insensitive_map_t<unique_ptr<CreateIndexInfo>> index_map;
	for (idx_t row = 0; row < result->Count(); row++) {
		auto idx_name = result->GetString(row, 0);
		auto tbl_name = result->GetString(row, 1);
		auto uniqueness = result->GetString(row, 2);
		auto col_name = result->GetString(row, 3);

		if (index_map.find(idx_name) == index_map.end()) {
			auto info = make_uniq<CreateIndexInfo>();
			info->index_name = idx_name;
			info->table = tbl_name;
			info->constraint_type = (uniqueness == "UNIQUE")
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

} // namespace duckdb
