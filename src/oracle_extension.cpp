#define DUCKDB_BUILD_LOADABLE_EXTENSION
#include "duckdb.hpp"

#include "oracle_scanner.hpp"
#include "oracle_storage.hpp"
#include "oracle_scanner_extension.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_optimizer.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/common/error_data.hpp"
#include "oracle_logging.hpp"

using namespace duckdb;

class OracleExtensionState : public ClientContextState {
public:
	bool CanRequestRebind() override {
		return true;
	}
	RebindQueryInfo OnPlanningError(ClientContext &context, SQLStatement &statement,
	                                 ErrorData &error) override {
		if (error.Type() != ExceptionType::BINDER) {
			return RebindQueryInfo::DO_NOT_REBIND;
		}
		auto &extra_info = error.ExtraInfo();
		auto entry = extra_info.find("error_subtype");
		if (entry == extra_info.end()) {
			return RebindQueryInfo::DO_NOT_REBIND;
		}
		if (entry->second != "COLUMN_NOT_FOUND") {
			return RebindQueryInfo::DO_NOT_REBIND;
		}
		OracleClearCacheFunction::ClearOracleCaches(context);
		return RebindQueryInfo::ATTEMPT_TO_REBIND;
	}
};

class OracleExtensionCallback : public ExtensionCallback {
public:
	void OnConnectionOpened(ClientContext &context) override {
		context.registered_state->Insert(
		    "oracle_extension", make_shared_ptr<OracleExtensionState>());
	}
};

static void SetOracleConnectionLimit(ClientContext &context, SetScope scope,
                                      Value &parameter) {
	if (scope == SetScope::LOCAL) {
		throw InvalidInputException("ora_connection_limit can only be set globally");
	}
	auto databases = DatabaseManager::Get(context).GetDatabases(context);
	for (auto &db_ref : databases) {
		auto &db = *db_ref;
		auto &catalog = db.GetCatalog();
		if (catalog.GetCatalogType() != "oracle") {
			continue;
		}
		catalog.Cast<OracleCatalog>().GetConnectionPool().SetMaximumConnections(
		    UBigIntValue::Get(parameter));
	}
	auto &config = DBConfig::GetConfig(context);
	config.SetOption("ora_connection_limit", parameter);
}

static void SetOracleDebugQueryPrint(ClientContext &context, SetScope scope,
                                      Value &parameter) {
	OracleConnection::DebugSetPrintQueries(BooleanValue::Get(parameter));
}

unique_ptr<BaseSecret> CreateOracleSecretFunction(ClientContext &context,
                                                   CreateSecretInput &input) {
	vector<string> prefix_paths;
	auto result =
	    make_uniq<KeyValueSecret>(prefix_paths, "oracle", "config", input.name);
	for (const auto &named_param : input.options) {
		auto lower_name = StringUtil::Lower(named_param.first);
		if (lower_name == "user" || lower_name == "username") {
			result->secret_map["user"] = named_param.second.ToString();
		} else if (lower_name == "password") {
			result->secret_map["password"] = named_param.second.ToString();
		} else if (lower_name == "connectstring" || lower_name == "host") {
			result->secret_map["connectString"] = named_param.second.ToString();
		} else {
			throw InternalException(
			    "Unknown named parameter passed to CreateOracleSecretFunction: " + lower_name);
		}
	}
	result->redact_keys = {"password"};
	return std::move(result);
}

void SetOracleSecretParameters(CreateSecretFunction &function) {
	function.named_parameters["user"] = LogicalType::VARCHAR;
	function.named_parameters["password"] = LogicalType::VARCHAR;
	function.named_parameters["connectString"] = LogicalType::VARCHAR;
	function.named_parameters["host"] = LogicalType::VARCHAR; // alias
}

static void LoadInternal(ExtensionLoader &loader) {
	OracleScanFunction oracle_fun;
	loader.RegisterFunction(oracle_fun);

	OracleScanFunctionFilterPushdown oracle_fun_filter_pushdown;
	loader.RegisterFunction(oracle_fun_filter_pushdown);

	OracleAttachFunction attach_func;
	loader.RegisterFunction(attach_func);

	OracleClearCacheFunction clear_cache_func;
	loader.RegisterFunction(clear_cache_func);

	OracleQueryFunction query_func;
	loader.RegisterFunction(query_func);

	OracleExecuteFunction execute_func;
	loader.RegisterFunction(execute_func);

	// Secret type
	SecretType secret_type;
	secret_type.name = "oracle";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	loader.RegisterSecretType(secret_type);

	CreateSecretFunction oracle_secret_function = {"oracle", "config",
	                                                CreateOracleSecretFunction};
	SetOracleSecretParameters(oracle_secret_function);
	loader.RegisterFunction(oracle_secret_function);

	// Storage extension
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.GetCallbackManager().Register("oracle", make_shared_ptr<OracleStorageExtension>());

	// Extension options
	config.AddExtensionOption("ora_connection_limit",
	                           "Maximum concurrent Oracle connections",
	                           LogicalType::UBIGINT,
	                           Value::UBIGINT(OracleConnectionPool::DEFAULT_MAX_CONNECTIONS),
	                           SetOracleConnectionLimit);
	config.AddExtensionOption("ora_connection_cache",
	                           "Whether to cache Oracle connections",
	                           LogicalType::BOOLEAN, Value::BOOLEAN(true),
	                           OracleConnectionPool::OracleSetConnectionCache);
	config.AddExtensionOption("ora_debug_show_queries",
	                           "DEBUG: print all Oracle queries to stdout",
	                           LogicalType::BOOLEAN, Value::BOOLEAN(false),
	                           SetOracleDebugQueryPrint);
	config.AddExtensionOption("ora_experimental_filter_pushdown",
	                           "Enable WHERE clause filter pushdown",
	                           LogicalType::BOOLEAN, Value::BOOLEAN(true));

	// Optimizer
	OptimizerExtension oracle_optimizer;
	oracle_optimizer.optimize_function = OracleOptimizer::Optimize;
	config.GetCallbackManager().Register(std::move(oracle_optimizer));

	// Extension callback
	config.GetCallbackManager().Register(make_shared_ptr<OracleExtensionCallback>());
	for (auto &connection :
	     ConnectionManager::Get(loader.GetDatabaseInstance()).GetConnectionList()) {
		connection->registered_state->Insert(
		    "oracle_extension", make_shared_ptr<OracleExtensionState>());
	}

	// Log type
	auto &instance = loader.GetDatabaseInstance();
	auto &log_manager = instance.GetLogManager();
	log_manager.RegisterLogType(make_uniq<OracleQueryLogType>());
}

void OracleScannerExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(oracle, loader) {
	LoadInternal(loader);
}
}
