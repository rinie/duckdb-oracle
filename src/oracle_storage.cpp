#include "duckdb.hpp"
#include "oracle_storage.hpp"
#include "storage/oracle_catalog.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "storage/oracle_transaction_manager.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

static unique_ptr<Catalog> OracleAttach(optional_ptr<StorageExtensionInfo> storage_info,
                                         ClientContext &context, AttachedDatabase &db,
                                         const string &name, AttachInfo &info,
                                         AttachOptions &attach_options) {
	if (!Settings::Get<EnableExternalAccessSetting>(DBConfig::GetConfig(context))) {
		throw PermissionException(
		    "Attaching Oracle databases is disabled through configuration");
	}
	string attach_path = info.path;

	string secret_name;
	string schema_to_load;
	OracleIsolationLevel isolation_level = OracleIsolationLevel::READ_COMMITTED;

	for (auto &entry : attach_options.options) {
		auto lower_name = StringUtil::Lower(entry.first);
		if (lower_name == "secret") {
			secret_name = entry.second.ToString();
		} else if (lower_name == "schema") {
			schema_to_load = entry.second.ToString();
		} else if (lower_name == "isolation_level") {
			auto param = StringUtil::Lower(entry.second.ToString());
			if (param == "read committed") {
				isolation_level = OracleIsolationLevel::READ_COMMITTED;
			} else if (param == "serializable") {
				isolation_level = OracleIsolationLevel::SERIALIZABLE;
			} else {
				throw InvalidInputException(
				    "Invalid isolation_level for Oracle: \"%s\" (use READ COMMITTED or SERIALIZABLE)",
				    entry.second.ToString());
			}
		} else {
			throw BinderException("Unrecognized option for Oracle attach: %s", entry.first);
		}
	}

	auto connection_string =
	    OracleCatalog::GetConnectionString(context, attach_path, secret_name);
	return make_uniq<OracleCatalog>(db, std::move(connection_string),
	                                 std::move(attach_path), attach_options.access_mode,
	                                 std::move(schema_to_load), isolation_level, context);
}

static unique_ptr<TransactionManager>
OracleCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                AttachedDatabase &db, Catalog &catalog) {
	auto &oracle_catalog = catalog.Cast<OracleCatalog>();
	return make_uniq<OracleTransactionManager>(db, oracle_catalog);
}

OracleStorageExtension::OracleStorageExtension() {
	attach = OracleAttach;
	create_transaction_manager = OracleCreateTransactionManager;
}

} // namespace duckdb
