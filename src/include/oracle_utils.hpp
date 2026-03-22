//===----------------------------------------------------------------------===//
//                         DuckDB
//
// oracle_utils.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include <dpi.h>

namespace duckdb {
class OracleSchemaEntry;
class OracleTransaction;

struct OracleTypeData {
	string type_name;     // DATA_TYPE from ALL_TAB_COLUMNS
	int64_t data_precision = -1; // DATA_PRECISION (NUMBER digits)
	int64_t data_scale = -127;   // DATA_SCALE (NUMBER decimal places)
	int64_t data_length = 0;     // DATA_LENGTH (char/byte length)
	int64_t char_length = 0;     // CHAR_LENGTH (character length)
};

enum class OracleTypeAnnotation {
	STANDARD,
	CAST_TO_VARCHAR,       // unsupported type → cast to VARCHAR
	NUMBER_AS_DOUBLE,      // unconstrained NUMBER → DOUBLE
	DATE_AS_TIMESTAMP,     // Oracle DATE includes time component
	CLOB_AS_VARCHAR,       // CLOB/NCLOB read as VARCHAR
	TIMESTAMP_WITH_TZ,     // TIMESTAMP WITH TIME ZONE
	TIMESTAMP_WITH_LTZ,    // TIMESTAMP WITH LOCAL TIME ZONE
};

struct OracleType {
	idx_t type_code = 0;
	OracleTypeAnnotation info = OracleTypeAnnotation::STANDARD;
	vector<OracleType> children;
};

enum class OracleIsolationLevel { READ_COMMITTED, SERIALIZABLE };

//! Controls which Oracle data-dictionary view family is used for schema introspection.
//!   USER  — USER_* views  : own schema only, no privilege checks, fastest (default)
//!   ALL   — ALL_*  views  : all accessible objects
//!   DBA   — DBA_*  views  : all DB objects, requires DBA role
enum class OraclePrivilegeLevel { USER, ALL, DBA };

struct OracleVersion {
	int major_v = 0;
	int minor_v = 0;
	int patch_v = 0;
	int port_v = 0;
	int port_patch_v = 0;
};

class OracleUtils {
public:
	//! Connect to Oracle using ODPI-C
	static dpiConn *OraConnect(const string &dsn, const string &attach_path);

	//! Parse DSN string: "user/pass@connect_string"
	//! OR key-value: "user=xxx password=yyy connectString=//host:port/svc"
	static void ParseDSN(const string &dsn, string &user, string &password,
	                      string &connect_string);

	//! Map Oracle ALL_TAB_COLUMNS type_info to DuckDB LogicalType
	static LogicalType TypeToLogicalType(const OracleTypeData &type_info,
	                                     OracleType &oracle_type);

	//! Map DuckDB LogicalType to Oracle SQL type string (for CREATE TABLE)
	static string TypeToString(const LogicalType &input);

	//! Map DuckDB LogicalType to its Oracle equivalent for INSERT/CREATE
	static LogicalType ToOracleType(const LogicalType &input);

	//! Quote an Oracle identifier with double quotes
	static string QuoteIdentifier(const string &text);

	//! Get the ODPI-C global context (created once per process)
	static dpiContext *GetOrCreateContext();

	//! Get Oracle version from a connection
	static OracleVersion GetVersion(dpiConn *conn);

	//! Write a quoted string literal for Oracle SQL (single quotes, escape single quotes)
	static string WriteLiteral(const string &value);

private:
	static dpiContext *g_dpi_context;
	static mutex g_context_mutex;
};

} // namespace duckdb
