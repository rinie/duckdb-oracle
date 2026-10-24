#include "duckdb.hpp"

#include "oracle_scanner.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "storage/oracle_catalog.hpp"

namespace duckdb {

struct ClearCacheFunctionData : public TableFunctionData {
	bool finished = false;
};

static unique_ptr<FunctionData> ClearCacheBind(ClientContext &context,
                                                TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types,
                                                vector<Identifier> &names) {
	auto result = make_uniq<ClearCacheFunctionData>();
	return_types.push_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");
	return std::move(result);
}

void OracleClearCacheFunction::ClearOracleCaches(ClientContext &context) {
	auto databases = DatabaseManager::Get(context).GetDatabases(context);
	for (auto &db_ref : databases) {
		auto &db = *db_ref;
		auto &catalog = db.GetCatalog();
		if (catalog.GetCatalogType() != "oracle") {
			continue;
		}
		catalog.Cast<OracleCatalog>().ClearCache();
	}
}

static void ClearCacheFunction(ClientContext &context, TableFunctionInput &data_p,
                                DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<ClearCacheFunctionData>();
	if (data.finished) {
		return;
	}
	OracleClearCacheFunction::ClearOracleCaches(context);
	data.finished = true;
}

void OracleClearCacheFunction::ClearCacheOnSetting(ClientContext &context, SetScope scope,
                                                    Value &parameter) {
	OracleClearCacheFunction::ClearOracleCaches(context);
}

OracleClearCacheFunction::OracleClearCacheFunction()
    : TableFunction("oracle_clear_cache", {}, ClearCacheFunction, ClearCacheBind) {
}

} // namespace duckdb
