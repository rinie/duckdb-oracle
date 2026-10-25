//===----------------------------------------------------------------------===//
//                         DuckDB
//
// oracle_connection.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "oracle_utils.hpp"
#include "oracle_result.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {
class OracleSchemaEntry;
class OracleTableEntry;

struct OwnedOracleConnection {
	explicit OwnedOracleConnection(dpiConn *conn = nullptr);
	OwnedOracleConnection(const OwnedOracleConnection &) = delete;
	OwnedOracleConnection &operator=(const OwnedOracleConnection &) = delete;
	~OwnedOracleConnection();

	dpiConn *connection;
	mutex connection_lock;
};

class OracleConnection {
public:
	explicit OracleConnection(shared_ptr<OwnedOracleConnection> connection = nullptr);
	~OracleConnection();
	// disable copy constructors
	OracleConnection(const OracleConnection &other) = delete;
	OracleConnection &operator=(const OracleConnection &) = delete;
	//! enable move constructors
	OracleConnection(OracleConnection &&other) noexcept;
	OracleConnection &operator=(OracleConnection &&) noexcept;

public:
	static OracleConnection Open(const string &dsn, const string &attach_path);

	void Execute(optional_ptr<ClientContext> context, const string &query);
	unique_ptr<OracleResult> TryQuery(optional_ptr<ClientContext> context,
	                                   const string &query,
	                                   optional_ptr<string> error_message = nullptr);
	unique_ptr<OracleResult> Query(optional_ptr<ClientContext> context,
	                                const string &query);

	//! Variants that bind named parameters (e.g. :owner, :table_name) via dpiStmt_bindValueByName
	unique_ptr<OracleResult> TryQuery(optional_ptr<ClientContext> context,
	                                   const string &query,
	                                   const unordered_map<string, string> &binds,
	                                   optional_ptr<string> error_message = nullptr);
	unique_ptr<OracleResult> Query(optional_ptr<ClientContext> context,
	                                const string &query,
	                                const unordered_map<string, string> &binds);

	void Commit();
	void Rollback();

	OracleVersion GetOracleVersion();

	bool IsOpen();
	void Close();

	bool IsConnectionOk();

	shared_ptr<OwnedOracleConnection> GetConnection() {
		return connection;
	}

	string GetDSN() {
		return dsn;
	}

	dpiConn *GetConn() {
		if (!connection || !connection->connection) {
			throw InternalException("OracleConnection::GetConn - no connection available");
		}
		return connection->connection;
	}

	static void DebugSetPrintQueries(bool print);
	static bool DebugPrintQueries();

private:
	//! Execute a query (with optional named bind params) and materialise results.
	//! A non-zero fetch_array_size makes ODPI-C fetch (and prefetch) that many rows per round trip
	//! instead of its default of 100.
	unique_ptr<OracleResult> ExecuteQuery(const string &query,
	                                       const unordered_map<string, string> &binds = {},
	                                       uint32_t fetch_array_size = 0);

	shared_ptr<OwnedOracleConnection> connection;
	string dsn;
};

} // namespace duckdb
