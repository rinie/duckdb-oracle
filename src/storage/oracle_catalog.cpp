#include "storage/oracle_catalog.hpp"
#include "storage/oracle_schema_entry.hpp"
#include "storage/oracle_transaction.hpp"
#include "oracle_connection.hpp"
#include "oracle_scanner.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"

namespace duckdb {

OracleCatalog::OracleCatalog(AttachedDatabase &db_p, string connection_string_p,
                               string attach_path_p, AccessMode access_mode,
                               string schema_to_load,
                               OracleIsolationLevel isolation_level,
                               OraclePrivilegeLevel privilege_level,
                               ClientContext &context)
    : Catalog(db_p), connection_string(std::move(connection_string_p)),
      attach_path(std::move(attach_path_p)), access_mode(access_mode),
      isolation_level(isolation_level), privilege_level(privilege_level),
      schemas(*this, schema_to_load), connection_pool(*this) {
	// Determine default schema from current user if not specified
	if (schema_to_load.empty()) {
		// Open a temp connection to get current user
		try {
			auto con = connection_pool.GetConnection();
			auto result = con.GetConnection().Query(context, "SELECT LOWER(USER) FROM DUAL");
			if (result && result->Count() > 0) {
				default_schema = result->GetString(0, 0);
			}
		} catch (...) {
			// fallback
		}
	} else {
		default_schema = StringUtil::Lower(schema_to_load);
	}

	// Get Oracle version
	auto con = connection_pool.GetConnection();
	version = con.GetConnection().GetOracleVersion();
}

OracleCatalog::~OracleCatalog() = default;

// ---- Connection string helpers ----

static string AddOracleConnectionOption(const KeyValueSecret &kv_secret,
                                         const string &name) {
	Value input_val = kv_secret.TryGetValue(name);
	if (input_val.IsNull()) {
		return string();
	}
	return name + "=" + input_val.ToString() + " ";
}

static unique_ptr<SecretEntry> GetSecret(ClientContext &context,
                                          const string &secret_name) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name, "memory");
	if (secret_entry) {
		return secret_entry;
	}
	secret_entry = secret_manager.GetSecretByName(transaction, secret_name, "local_file");
	return secret_entry;
}

string OracleCatalog::GetConnectionString(ClientContext &context,
                                           const string &attach_path,
                                           string secret_name) {
	string connection_string = attach_path;
	bool explicit_secret = !secret_name.empty();
	if (!explicit_secret) {
		secret_name = "__default_oracle";
	}
	auto secret_entry = GetSecret(context, secret_name);
	if (secret_entry) {
		const auto &kv_secret = dynamic_cast<const KeyValueSecret &>(*secret_entry->secret);
		string user = kv_secret.TryGetValue("user").ToString();
		string password = kv_secret.TryGetValue("password").ToString();
		string conn_str = kv_secret.TryGetValue("connectString").ToString();
		if (!user.empty() && !password.empty()) {
			// Reconstruct as user/password@conn_str
			connection_string = user + "/" + password + "@" + conn_str;
		}
	} else if (explicit_secret) {
		throw BinderException("Secret with name \"%s\" not found", secret_name);
	}
	return connection_string;
}

void OracleCatalog::Initialize(bool load_builtin) {
}

optional_ptr<CatalogEntry> OracleCatalog::CreateSchema(CatalogTransaction transaction,
                                                        CreateSchemaInfo &info) {
	auto &oracle_transaction = OracleTransaction::Get(transaction.GetContext(), *this);
	auto entry = schemas.GetEntry(transaction.GetContext(), oracle_transaction, info.schema);
	if (entry) {
		switch (info.on_conflict) {
		case OnCreateConflict::REPLACE_ON_CONFLICT: {
			DropInfo try_drop;
			try_drop.type = CatalogType::SCHEMA_ENTRY;
			try_drop.name = info.schema;
			try_drop.if_not_found = OnEntryNotFound::RETURN_NULL;
			try_drop.cascade = false;
			schemas.DropEntry(oracle_transaction, try_drop);
			break;
		}
		case OnCreateConflict::IGNORE_ON_CONFLICT:
			return entry;
		case OnCreateConflict::ERROR_ON_CONFLICT:
		default:
			throw BinderException("Failed to create schema \"%s\": schema already exists",
			                       info.schema);
		}
	}
	// In Oracle, schemas ARE users; we don't create them here.
	// Just register a virtual entry.
	auto schema_entry = make_shared_ptr<OracleSchemaEntry>(*this, info);
	return schemas.CreateEntry(oracle_transaction, std::move(schema_entry));
}

void OracleCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	auto &oracle_transaction = OracleTransaction::Get(context, *this);
	schemas.DropEntry(oracle_transaction, info);
}

void OracleCatalog::ScanSchemas(ClientContext &context,
                                  std::function<void(SchemaCatalogEntry &)> callback) {
	auto &oracle_transaction = OracleTransaction::Get(context, *this);
	schemas.Scan(context, oracle_transaction,
	              [&](CatalogEntry &schema) { callback(schema.Cast<OracleSchemaEntry>()); });
}

optional_ptr<SchemaCatalogEntry> OracleCatalog::LookupSchema(
    CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
    OnEntryNotFound if_not_found) {
	auto schema_name = schema_lookup.GetEntryName();
	auto &oracle_transaction = OracleTransaction::Get(transaction.GetContext(), *this);
	auto entry = schemas.GetEntry(transaction.GetContext(), oracle_transaction, schema_name);
	if (!entry && if_not_found != OnEntryNotFound::RETURN_NULL) {
		throw BinderException("Schema with name \"%s\" not found", schema_name);
	}
	return reinterpret_cast<SchemaCatalogEntry *>(entry.get());
}

bool OracleCatalog::InMemory() {
	return false;
}

string OracleCatalog::GetDBPath() {
	return attach_path;
}

DatabaseSize OracleCatalog::GetDatabaseSize(ClientContext &context) {
	auto &oracle_transaction = OracleTransaction::Get(context, *this);
	// Sum allocated bytes from user's extents
	auto result = oracle_transaction.Query(
	    "SELECT NVL(SUM(bytes),0) FROM user_extents");
	DatabaseSize size;
	size.free_blocks = 0;
	size.total_blocks = 0;
	size.used_blocks = 0;
	size.wal_size = 0;
	size.block_size = 0;
	if (result && result->Count() > 0 && !result->IsNull(0, 0)) {
		size.bytes = result->GetInt64(0, 0);
	}
	return size;
}

void OracleCatalog::ClearCache() {
	schemas.ClearEntries();
}

bool OracleCatalog::IsOracleScan(const string &name) {
	return name == "oracle_scan" || name == "oracle_scan_pushdown" ||
	       name == "oracle_query";
}

void OracleCatalog::MaterializeOracleScans(PhysicalOperator &op) {
	if (op.type == PhysicalOperatorType::TABLE_SCAN) {
		auto &table_scan = op.Cast<PhysicalTableScan>();
		if (OracleCatalog::IsOracleScan(table_scan.function.name)) {
			auto &bind_data = table_scan.bind_data->Cast<OracleBindData>();
			bind_data.requires_materialization = true;
			bind_data.max_threads = 1;
			bind_data.can_use_main_thread = true;
		}
	}
	for (auto &child : op.children) {
		MaterializeOracleScans(child);
	}
}

} // namespace duckdb
