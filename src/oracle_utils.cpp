#include "oracle_utils.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

#include <cctype>
#include <cmath>
#include <cstring>

namespace duckdb {

dpiContext *OracleUtils::g_dpi_context = nullptr;
mutex OracleUtils::g_context_mutex;

dpiContext *OracleUtils::GetOrCreateContext() {
	lock_guard<mutex> l(g_context_mutex);
	if (g_dpi_context) {
		return g_dpi_context;
	}
	dpiErrorInfo error_info;
	dpiContextCreateParams params;
	memset(&params, 0, sizeof(params));
	if (dpiContext_createWithParams(DPI_MAJOR_VERSION, DPI_MINOR_VERSION, &params,
	                                &g_dpi_context, &error_info) < 0) {
		throw IOException("Failed to create ODPI-C context: %s (fn=%s)",
		                  error_info.message, error_info.fnName);
	}
	return g_dpi_context;
}

void OracleUtils::ParseDSN(const string &dsn, string &user, string &password,
                            string &connect_string) {
	// Try key-value format first: "user=xxx password=yyy connectString=//..."
	if (dsn.find('=') != string::npos && dsn.find('/') == string::npos) {
		// Simple key=value parser
		auto parts = StringUtil::Split(dsn, ' ');
		for (auto &part : parts) {
			auto eq = part.find('=');
			if (eq == string::npos) {
				continue;
			}
			auto key = StringUtil::Lower(part.substr(0, eq));
			auto val = part.substr(eq + 1);
			if (key == "user" || key == "username") {
				user = val;
			} else if (key == "password" || key == "pass") {
				password = val;
			} else if (key == "connectstring" || key == "dsn" || key == "host") {
				connect_string = val;
			}
		}
		return;
	}

	// Oracle format: "user/password@connect_string"
	// OR: "user/password" (no connect string - uses ORACLE_SID/TWO_TASK env)
	auto at_pos = dsn.rfind('@');
	if (at_pos != string::npos) {
		connect_string = dsn.substr(at_pos + 1);
		auto user_pass = dsn.substr(0, at_pos);
		auto slash_pos = user_pass.find('/');
		if (slash_pos != string::npos) {
			user = user_pass.substr(0, slash_pos);
			password = user_pass.substr(slash_pos + 1);
		} else {
			user = user_pass;
		}
	} else {
		// No @ sign - entire string might be just user/pass or user
		auto slash_pos = dsn.find('/');
		if (slash_pos != string::npos) {
			user = dsn.substr(0, slash_pos);
			password = dsn.substr(slash_pos + 1);
		} else {
			// Use as connect string, rely on external auth or wallets
			connect_string = dsn;
		}
	}
}

dpiConn *OracleUtils::OraConnect(const string &dsn, const string &attach_path) {
	auto context = GetOrCreateContext();

	string user, password, connect_string;
	ParseDSN(dsn, user, password, connect_string);

	dpiConn *conn = nullptr;
	dpiErrorInfo error_info;

	dpiConnCreateParams conn_params;
	dpiContext_initConnCreateParams(context, &conn_params);

	if (dpiConn_create(context, user.c_str(), (uint32_t)user.size(), password.c_str(),
	                   (uint32_t)password.size(), connect_string.c_str(),
	                   (uint32_t)connect_string.size(), nullptr, &conn_params,
	                   &conn) < 0) {
		dpiContext_getError(context, &error_info);
		throw IOException("Unable to connect to Oracle at \"%s\": %s", attach_path,
		                  string(error_info.message));
	}
	return conn;
}

OracleVersion OracleUtils::GetVersion(dpiConn *conn) {
	OracleVersion version;
	dpiVersionInfo ver_info;
	if (dpiConn_getServerVersion(conn, nullptr, nullptr, &ver_info) < 0) {
		return version;
	}
	version.major_v = ver_info.versionNum;
	version.minor_v = ver_info.releaseNum;
	version.patch_v = ver_info.updateNum;
	version.port_v = ver_info.portReleaseNum;
	version.port_patch_v = ver_info.portUpdateNum;
	return version;
}

LogicalType OracleUtils::TypeToLogicalType(const OracleTypeData &type_info,
                                            OracleType &oracle_type) {
	auto type_name = StringUtil::Upper(type_info.type_name);

	// Strip inline precision from type name while preserving suffixes.
	// e.g. "TIMESTAMP(6)"                   -> "TIMESTAMP"
	//      "TIMESTAMP(6) WITH TIME ZONE"     -> "TIMESTAMP WITH TIME ZONE"
	//      "TIMESTAMP(6) WITH LOCAL TIME ZONE" -> "TIMESTAMP WITH LOCAL TIME ZONE"
	string base_type;
	auto paren_pos = type_name.find('(');
	if (paren_pos != string::npos) {
		auto close_pos = type_name.find(')', paren_pos);
		if (close_pos != string::npos && close_pos + 1 < type_name.size()) {
			// Rejoin: everything before '(' + everything after ')'
			base_type = type_name.substr(0, paren_pos) + type_name.substr(close_pos + 1);
		} else {
			base_type = type_name.substr(0, paren_pos);
		}
	} else {
		base_type = type_name;
	}
	StringUtil::Trim(base_type);

	if (base_type == "NUMBER" || base_type == "NUMERIC" || base_type == "DECIMAL") {
		// NUMBER with precision and scale
		if (type_info.data_precision > 0 && type_info.data_scale >= 0) {
			if (type_info.data_scale == 0 && type_info.data_precision <= 18) {
				// Integer-like number
				if (type_info.data_precision <= 4) {
					return LogicalType::SMALLINT;
				} else if (type_info.data_precision <= 9) {
					return LogicalType::INTEGER;
				} else {
					return LogicalType::BIGINT;
				}
			}
			if (type_info.data_precision <= 38 && type_info.data_scale >= 0) {
				return LogicalType::DECIMAL((uint8_t)type_info.data_precision,
				                            (uint8_t)type_info.data_scale);
			}
		}
		// Unconstrained NUMBER -> DOUBLE
		oracle_type.info = OracleTypeAnnotation::NUMBER_AS_DOUBLE;
		return LogicalType::DOUBLE;
	} else if (base_type == "FLOAT") {
		// Oracle FLOAT is NUMBER with a binary precision - map to DOUBLE
		return LogicalType::DOUBLE;
	} else if (base_type == "BINARY_FLOAT") {
		return LogicalType::FLOAT;
	} else if (base_type == "BINARY_DOUBLE") {
		return LogicalType::DOUBLE;
	} else if (base_type == "INTEGER" || base_type == "INT" || base_type == "SMALLINT") {
		return LogicalType::BIGINT;
	} else if (base_type == "VARCHAR2" || base_type == "NVARCHAR2" || base_type == "VARCHAR") {
		return LogicalType::VARCHAR;
	} else if (base_type == "CHAR" || base_type == "NCHAR") {
		return LogicalType::VARCHAR;
	} else if (base_type == "CLOB" || base_type == "NCLOB" || base_type == "LONG") {
		oracle_type.info = OracleTypeAnnotation::CLOB_AS_VARCHAR;
		return LogicalType::VARCHAR;
	} else if (base_type == "BLOB") {
		return LogicalType::BLOB;
	} else if (base_type == "RAW" || base_type == "LONG RAW") {
		return LogicalType::BLOB;
	} else if (base_type == "DATE") {
		// Oracle DATE includes time: map to TIMESTAMP
		oracle_type.info = OracleTypeAnnotation::DATE_AS_TIMESTAMP;
		return LogicalType::TIMESTAMP;
	} else if (base_type == "TIMESTAMP") {
		return LogicalType::TIMESTAMP;
	} else if (base_type == "TIMESTAMP WITH TIME ZONE") {
		oracle_type.info = OracleTypeAnnotation::TIMESTAMP_WITH_TZ;
		return LogicalType::TIMESTAMP_TZ;
	} else if (base_type == "TIMESTAMP WITH LOCAL TIME ZONE") {
		oracle_type.info = OracleTypeAnnotation::TIMESTAMP_WITH_LTZ;
		return LogicalType::TIMESTAMP;
	} else if (base_type == "INTERVAL DAY TO SECOND" ||
	           StringUtil::StartsWith(base_type, "INTERVAL DAY")) {
		return LogicalType::INTERVAL;
	} else if (base_type == "INTERVAL YEAR TO MONTH" ||
	           StringUtil::StartsWith(base_type, "INTERVAL YEAR")) {
		// Map year-month intervals to VARCHAR (DuckDB INTERVAL is day-time based)
		oracle_type.info = OracleTypeAnnotation::CAST_TO_VARCHAR;
		return LogicalType::VARCHAR;
	} else if (base_type == "XMLTYPE") {
		oracle_type.info = OracleTypeAnnotation::CAST_TO_VARCHAR;
		return LogicalType::VARCHAR;
	} else if (base_type == "ROWID" || base_type == "UROWID") {
		oracle_type.info = OracleTypeAnnotation::CAST_TO_VARCHAR;
		return LogicalType::VARCHAR;
	} else if (base_type == "SDO_GEOMETRY" || base_type == "MDSYS.SDO_GEOMETRY") {
		// Oracle Spatial geometry - cast to VARCHAR
		oracle_type.info = OracleTypeAnnotation::CAST_TO_VARCHAR;
		return LogicalType::VARCHAR;
	} else if (base_type == "BOOLEAN") {
		// Oracle 23c+ BOOLEAN; older Oracle lacks this
		return LogicalType::BOOLEAN;
	} else {
		// Unknown/unsupported type - cast to VARCHAR
		oracle_type.info = OracleTypeAnnotation::CAST_TO_VARCHAR;
		return LogicalType::VARCHAR;
	}
}

string OracleUtils::TypeToString(const LogicalType &input) {
	switch (input.id()) {
	case LogicalTypeId::BOOLEAN:
		return "NUMBER(1)"; // Oracle 12c/18c: use NUMBER(1) for boolean
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
		return "NUMBER(5)";
	case LogicalTypeId::INTEGER:
		return "NUMBER(10)";
	case LogicalTypeId::BIGINT:
		return "NUMBER(19)";
	case LogicalTypeId::UTINYINT:
		return "NUMBER(3)";
	case LogicalTypeId::USMALLINT:
		return "NUMBER(5)";
	case LogicalTypeId::UINTEGER:
		return "NUMBER(10)";
	case LogicalTypeId::UBIGINT:
		return "NUMBER(20)";
	case LogicalTypeId::FLOAT:
		return "BINARY_FLOAT";
	case LogicalTypeId::DOUBLE:
		return "BINARY_DOUBLE";
	case LogicalTypeId::DECIMAL: {
		auto width = DecimalType::GetWidth(input);
		auto scale = DecimalType::GetScale(input);
		return StringUtil::Format("NUMBER(%d,%d)", width, scale);
	}
	case LogicalTypeId::VARCHAR:
		return "CLOB";
	case LogicalTypeId::BLOB:
		return "BLOB";
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		return "TIMESTAMP(6)";
	case LogicalTypeId::TIMESTAMP_TZ:
		return "TIMESTAMP(6) WITH TIME ZONE";
	case LogicalTypeId::TIME:
		return "VARCHAR2(15)"; // Oracle has no TIME type
	case LogicalTypeId::TIME_TZ:
		return "VARCHAR2(21)";
	case LogicalTypeId::INTERVAL:
		return "INTERVAL DAY(9) TO SECOND(6)";
	case LogicalTypeId::HUGEINT:
		return "NUMBER(39)";
	case LogicalTypeId::UUID:
		return "VARCHAR2(36)";
	default:
		return "CLOB";
	}
}

LogicalType OracleUtils::ToOracleType(const LogicalType &input) {
	switch (input.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::UUID:
		return input;
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		return LogicalType::TIMESTAMP;
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_TZ:
		return LogicalType::VARCHAR;
	case LogicalTypeId::HUGEINT:
		return LogicalType::DOUBLE;
	default:
		return LogicalType::VARCHAR;
	}
}

string OracleUtils::QuoteIdentifier(const string &text) {
	// Never quote Oracle identifiers — Oracle uppercases unquoted identifiers,
	// which matches how names are stored in the data dictionary.
	return text;
}

string OracleUtils::WriteLiteral(const string &value) {
	string result = "'";
	for (char c : value) {
		if (c == '\'') {
			result += "''";
		} else {
			result += c;
		}
	}
	result += "'";
	return result;
}

//! "TIMESTAMP 'YYYY-MM-DD HH24:MI:SS.FF6'" for a timestamp
static string TimestampLiteral(timestamp_t ts) {
	auto d = Timestamp::GetDate(ts);
	auto t = Timestamp::GetTime(ts);
	int32_t y, mo, day, h, m, s, micros;
	Date::Convert(d, y, mo, day);
	Time::Convert(t, h, m, s, micros);
	return StringUtil::Format("TIMESTAMP '%04d-%02d-%02d %02d:%02d:%02d.%06d'", y, mo, day, h, m, s, micros);
}

//! A text literal. SQL string literals are limited to 4000 bytes, so longer text is sent as
//! a concatenation of TO_CLOB(...) pieces.
static string TextLiteral(const string &text) {
	auto literal = OracleUtils::WriteLiteral(text);
	if (literal.size() <= 4000) {
		return literal;
	}
	string result;
	// 1500 bytes escape to at most 3002 bytes
	static constexpr size_t CHUNK_BYTES = 1500;
	size_t pos = 0;
	while (pos < text.size()) {
		size_t len = MinValue<size_t>(CHUNK_BYTES, text.size() - pos);
		// do not split a UTF-8 sequence
		while (pos + len < text.size() && len > 1 && (static_cast<unsigned char>(text[pos + len]) & 0xC0) == 0x80) {
			len--;
		}
		if (!result.empty()) {
			result += " || ";
		}
		result += "TO_CLOB(" + OracleUtils::WriteLiteral(text.substr(pos, len)) + ")";
		pos += len;
	}
	return result;
}

string OracleUtils::ValueToOracleSQL(const Value &val) {
	if (val.IsNull()) {
		return "NULL";
	}
	switch (val.type().id()) {
	case LogicalTypeId::BOOLEAN:
		return BooleanValue::Get(val) ? "1" : "0";
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::DECIMAL:
		// numeric literal, not a string that Oracle would have to convert using its NLS settings
		return val.ToString();
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE: {
		auto d = val.GetValue<double>();
		if (!std::isfinite(d)) {
			return WriteLiteral(val.ToString());
		}
		return val.ToString();
	}
	case LogicalTypeId::VARCHAR:
		return TextLiteral(StringValue::Get(val));
	case LogicalTypeId::BLOB: {
		// hex-encode the bytes for HEXTORAW(...); RAW (and BLOB, implicitly) columns take it
		auto raw = StringValue::Get(val);
		string hex;
		static const char hx[] = "0123456789ABCDEF";
		for (uint8_t b : raw) {
			hex += hx[b >> 4];
			hex += hx[b & 0xF];
		}
		return "HEXTORAW('" + hex + "')";
	}
	case LogicalTypeId::DATE: {
		auto d = DateValue::Get(val);
		int32_t y, mo, day;
		Date::Convert(d, y, mo, day);
		return StringUtil::Format("DATE '%04d-%02d-%02d'", y, mo, day);
	}
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		return TimestampLiteral(val.DefaultCastAs(LogicalType::TIMESTAMP).GetValueUnsafe<timestamp_t>());
	case LogicalTypeId::TIMESTAMP_TZ:
		// the stored value is a UTC instant; a plain string would depend on NLS_TIMESTAMP_TZ_FORMAT
		return "FROM_TZ(" + TimestampLiteral(val.GetValueUnsafe<timestamp_t>()) + ", '+00:00')";
	case LogicalTypeId::INTERVAL: {
		auto interval = val.GetValueUnsafe<interval_t>();
		if (interval.months != 0) {
			throw InvalidInputException(
			    "An INTERVAL with a month component cannot be stored in an Oracle INTERVAL DAY TO SECOND");
		}
		static constexpr int64_t MICROS_PER_DAY = 86400000000LL;
		int64_t total = static_cast<int64_t>(interval.days) * MICROS_PER_DAY + interval.micros;
		bool negative = total < 0;
		uint64_t abs_total = negative ? static_cast<uint64_t>(-total) : static_cast<uint64_t>(total);
		uint64_t days = abs_total / MICROS_PER_DAY;
		uint64_t rest = abs_total % MICROS_PER_DAY;
		return StringUtil::Format("INTERVAL '%s%llu %02llu:%02llu:%02llu.%06llu' DAY(9) TO SECOND(6)",
		                          negative ? "-" : "", (unsigned long long)days,
		                          (unsigned long long)(rest / 3600000000ULL),
		                          (unsigned long long)(rest / 60000000ULL % 60),
		                          (unsigned long long)(rest / 1000000ULL % 60), (unsigned long long)(rest % 1000000ULL));
	}
	default:
		return TextLiteral(val.ToString());
	}
}

void OracleUtils::DefineLobsAsBytes(dpiStmt *stmt, uint32_t num_cols) {
	// A size above ODPI-C's basic buffer limit makes the variable dynamic: values of any length
	// are fetched in pieces and nothing of that size is allocated up front.
	static constexpr uint32_t DYNAMIC_SIZE = 1U << 30;
	for (uint32_t pos = 1; pos <= num_cols; pos++) {
		dpiQueryInfo info;
		if (dpiStmt_getQueryInfo(stmt, pos, &info) < 0) {
			continue;
		}
		switch (info.typeInfo.oracleTypeNum) {
		case DPI_ORACLE_TYPE_CLOB:
		case DPI_ORACLE_TYPE_NCLOB:
			dpiStmt_defineValue(stmt, pos, DPI_ORACLE_TYPE_LONG_VARCHAR, DPI_NATIVE_TYPE_BYTES, DYNAMIC_SIZE, 1,
			                    nullptr);
			break;
		case DPI_ORACLE_TYPE_BLOB:
			dpiStmt_defineValue(stmt, pos, DPI_ORACLE_TYPE_LONG_RAW, DPI_NATIVE_TYPE_BYTES, DYNAMIC_SIZE, 1, nullptr);
			break;
		default:
			break;
		}
	}
}

} // namespace duckdb
