#include "oracle_connection.hpp"
#include "oracle_logging.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/interval.hpp"

#include <chrono>
#include <cmath>

namespace duckdb {

static bool debug_oracle_print_queries = false;

// ---------------------------------------------------------------------------
// OwnedOracleConnection
// ---------------------------------------------------------------------------

OwnedOracleConnection::OwnedOracleConnection(dpiConn *conn) : connection(conn) {
}

OwnedOracleConnection::~OwnedOracleConnection() {
	if (!connection) {
		return;
	}
	dpiConn_release(connection);
	connection = nullptr;
}

// ---------------------------------------------------------------------------
// OracleConnection
// ---------------------------------------------------------------------------

OracleConnection::OracleConnection(shared_ptr<OwnedOracleConnection> connection_p)
    : connection(std::move(connection_p)) {
}

OracleConnection::~OracleConnection() {
	Close();
}

OracleConnection::OracleConnection(OracleConnection &&other) noexcept {
	std::swap(connection, other.connection);
	std::swap(dsn, other.dsn);
}

OracleConnection &OracleConnection::operator=(OracleConnection &&other) noexcept {
	std::swap(connection, other.connection);
	std::swap(dsn, other.dsn);
	return *this;
}

OracleConnection OracleConnection::Open(const string &dsn, const string &attach_path) {
	OracleConnection result;
	result.connection =
	    make_shared_ptr<OwnedOracleConnection>(OracleUtils::OraConnect(dsn, attach_path));
	result.dsn = dsn;
	return result;
}

// ---------------------------------------------------------------------------
// Internal query execution: materialise all rows into OracleResult
// ---------------------------------------------------------------------------

unique_ptr<OracleResult> OracleConnection::ExecuteQuery(const string &query,
                                                         const unordered_map<string, string> &binds,
                                                         uint32_t fetch_array_size) {
	auto conn = GetConn();
	dpiStmt *stmt = nullptr;
	dpiErrorInfo error_info;

	auto result = make_uniq<OracleResult>();

	if (dpiConn_prepareStmt(conn, 0, query.c_str(), (uint32_t)query.size(),
	                         nullptr, 0, &stmt) < 0) {
		dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
		throw IOException("Oracle prepare failed for query \"%s\": %s", query,
		                  string(error_info.message));
	}

	// Bind named parameters (e.g. :owner, :table_name) by name.
	// Keys in |binds| must match the placeholder name without the leading colon.
	for (const auto &kv : binds) {
		const string &name  = kv.first;
		const string &value = kv.second;
		dpiData bind_data;
		// dpiData_setBytes sets isNull=0 and the asBytes fields.
		dpiData_setBytes(&bind_data, const_cast<char *>(value.c_str()),
		                 (uint32_t)value.size());
		if (dpiStmt_bindValueByName(stmt, name.c_str(), (uint32_t)name.size(),
		                             DPI_NATIVE_TYPE_BYTES, &bind_data) < 0) {
			dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
			dpiStmt_release(stmt);
			throw IOException("Oracle bind by name failed for ':%s': %s", name,
			                  string(error_info.message));
		}
	}

	if (fetch_array_size > 0) {
		// Best effort: fewer round trips for large results. On failure ODPI-C keeps its defaults.
		dpiStmt_setFetchArraySize(stmt, fetch_array_size);
		dpiStmt_setPrefetchRows(stmt, fetch_array_size);
	}

	uint32_t num_cols = 0;
	if (dpiStmt_execute(stmt, DPI_MODE_EXEC_DEFAULT, &num_cols) < 0) {
		dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
		dpiStmt_release(stmt);
		throw IOException("Oracle execute failed for query \"%s\": %s", query,
		                  string(error_info.message));
	}

	if (num_cols == 0) {
		// DML statement
		uint64_t row_count = 0;
		dpiStmt_getRowCount(stmt, &row_count);
		result->affected_rows = row_count;
		dpiStmt_release(stmt);
		return result;
	}

	// CLOB/BLOB columns are fetched as text/bytes instead of LOB locators
	OracleUtils::DefineLobsAsBytes(stmt, num_cols);

	// SELECT: fetch all rows
	int found = 0;
	uint32_t buffer_row_index = 0;
	while (true) {
		if (dpiStmt_fetch(stmt, &found, &buffer_row_index) < 0) {
			dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
			dpiStmt_release(stmt);
			throw IOException("Oracle fetch failed for query \"%s\": %s", query,
			                  string(error_info.message));
		}
		if (!found) {
			break;
		}
		vector<OracleResultCell> row(num_cols);
		for (uint32_t c = 1; c <= num_cols; c++) {
			dpiNativeTypeNum native_type;
			dpiData *data;
			if (dpiStmt_getQueryValue(stmt, c, &native_type, &data) < 0) {
				dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
				dpiStmt_release(stmt);
				throw IOException("Oracle getQueryValue failed: %s", string(error_info.message));
			}
			auto &cell = row[c - 1];
			if (data->isNull) {
				cell.is_null = true;
				continue;
			}
			cell.is_null = false;
			// Convert to string representation for the result
			switch (native_type) {
			case DPI_NATIVE_TYPE_INT64:
				cell.value = to_string(data->value.asInt64);
				break;
			case DPI_NATIVE_TYPE_UINT64:
				cell.value = to_string(data->value.asUint64);
				break;
			case DPI_NATIVE_TYPE_FLOAT:
				cell.value = to_string(data->value.asFloat);
				break;
			case DPI_NATIVE_TYPE_DOUBLE:
				cell.value = to_string(data->value.asDouble);
				break;
			case DPI_NATIVE_TYPE_BYTES:
				cell.value = string(data->value.asBytes.ptr, data->value.asBytes.length);
				break;
			case DPI_NATIVE_TYPE_TIMESTAMP: {
				auto &ts = data->value.asTimestamp;
				char buf[64];
				snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%06d",
				         ts.year, ts.month, ts.day, ts.hour, ts.minute, ts.second,
				         ts.fsecond / 1000);
				cell.value = buf;
				break;
			}
			case DPI_NATIVE_TYPE_INTERVAL_DS: {
				auto &ivl = data->value.asIntervalDS;
				char buf[64];
				snprintf(buf, sizeof(buf), "%d %02d:%02d:%02d.%06d", ivl.days,
				         ivl.hours, ivl.minutes, ivl.seconds, ivl.fseconds / 1000);
				cell.value = buf;
				break;
			}
			case DPI_NATIVE_TYPE_INTERVAL_YM: {
				auto &ivl = data->value.asIntervalYM;
				char buf[32];
				snprintf(buf, sizeof(buf), "%d-%d", ivl.years, ivl.months);
				cell.value = buf;
				break;
			}
			case DPI_NATIVE_TYPE_BOOLEAN:
				cell.value = data->value.asBoolean ? "1" : "0";
				break;
			default:
				cell.value = "";
				cell.is_null = true;
				break;
			}
		}
		result->rows.push_back(std::move(row));
	}

	dpiStmt_release(stmt);
	return result;
}

// ---------------------------------------------------------------------------
// Public query API
// ---------------------------------------------------------------------------

unique_ptr<OracleResult> OracleConnection::TryQuery(optional_ptr<ClientContext> context,
                                                     const string &query,
                                                     optional_ptr<string> error_message) {
	lock_guard<mutex> guard(connection->connection_lock);
	if (DebugPrintQueries()) {
		Printer::Print(query + "\n");
	}
	try {
		return ExecuteQuery(query);
	} catch (std::exception &ex) {
		if (error_message) {
			*error_message = ex.what();
		}
		return nullptr;
	}
}

unique_ptr<OracleResult> OracleConnection::Query(optional_ptr<ClientContext> context,
                                                   const string &query) {
	string error_msg;
	auto result = TryQuery(context, query, &error_msg);
	if (!result) {
		throw std::runtime_error(error_msg);
	}
	return result;
}

void OracleConnection::Execute(optional_ptr<ClientContext> context, const string &query) {
	Query(context, query);
}

// ---------------------------------------------------------------------------
// Array binding
// ---------------------------------------------------------------------------

namespace {

//! Longest text or byte value that is bound as a plain VARCHAR / RAW variable
constexpr idx_t MAX_BIND_BYTES = 32767;

struct BatchVars {
	~BatchVars() {
		for (auto var : vars) {
			dpiVar_release(var);
		}
	}
	vector<dpiVar *> vars;
};

[[noreturn]] void ThrowDpiError(const char *what) {
	dpiErrorInfo error_info;
	dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
	throw IOException("Oracle %s failed: %s", what, string(error_info.message));
}

void SetTimestamp(dpiData &data, timestamp_t ts, int8_t tz_hour) {
	int32_t y, mo, day, h, m, s, micros;
	Date::Convert(Timestamp::GetDate(ts), y, mo, day);
	Time::Convert(Timestamp::GetTime(ts), h, m, s, micros);
	dpiData_setTimestamp(&data, (int16_t)y, (uint8_t)mo, (uint8_t)day, (uint8_t)h, (uint8_t)m, (uint8_t)s,
	                     (uint32_t)micros * 1000, tz_hour, 0);
}

//! Creates the variable for one column and fills it. Returns false if a value cannot be bound.
bool BindColumn(dpiConn *conn, const LogicalType &type, const vector<Value> &values, dpiVar *&var, dpiData *&data) {
	dpiOracleTypeNum oracle_type;
	dpiNativeTypeNum native_type;
	uint32_t size = 0;
	// text and bytes are sized for the longest value; empty text is NULL in Oracle anyway
	vector<string> texts;

	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
		oracle_type = DPI_ORACLE_TYPE_NUMBER;
		native_type = DPI_NATIVE_TYPE_INT64;
		break;
	case LogicalTypeId::UBIGINT:
		oracle_type = DPI_ORACLE_TYPE_NUMBER;
		native_type = DPI_NATIVE_TYPE_UINT64;
		break;
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
		// ODPI-C converts the decimal text itself, independent of the NLS settings
		oracle_type = DPI_ORACLE_TYPE_NUMBER;
		native_type = DPI_NATIVE_TYPE_BYTES;
		break;
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::UUID:
	case LogicalTypeId::BLOB:
		oracle_type = type.id() == LogicalTypeId::BLOB ? DPI_ORACLE_TYPE_RAW : DPI_ORACLE_TYPE_VARCHAR;
		native_type = DPI_NATIVE_TYPE_BYTES;
		for (auto &val : values) {
			string text;
			if (!val.IsNull()) {
				text = type.id() == LogicalTypeId::UUID ? val.ToString() : StringValue::Get(val);
			}
			if (text.size() > MAX_BIND_BYTES) {
				return false;
			}
			size = MaxValue<uint32_t>(size, (uint32_t)text.size());
			texts.push_back(std::move(text));
		}
		break;
	case LogicalTypeId::DATE:
		oracle_type = DPI_ORACLE_TYPE_DATE;
		native_type = DPI_NATIVE_TYPE_TIMESTAMP;
		break;
	case LogicalTypeId::TIMESTAMP:
		oracle_type = DPI_ORACLE_TYPE_TIMESTAMP;
		native_type = DPI_NATIVE_TYPE_TIMESTAMP;
		break;
	case LogicalTypeId::TIMESTAMP_TZ:
		oracle_type = DPI_ORACLE_TYPE_TIMESTAMP_TZ;
		native_type = DPI_NATIVE_TYPE_TIMESTAMP;
		break;
	case LogicalTypeId::INTERVAL:
		oracle_type = DPI_ORACLE_TYPE_INTERVAL_DS;
		native_type = DPI_NATIVE_TYPE_INTERVAL_DS;
		break;
	default:
		return false;
	}

	if (dpiConn_newVar(conn, oracle_type, native_type, (uint32_t)values.size(), size == 0 ? 1 : size, 1, 0, nullptr,
	                   &var, &data) < 0) {
		ThrowDpiError("bind variable creation");
	}

	for (idx_t row = 0; row < values.size(); row++) {
		auto &val = values[row];
		auto &target = data[row];
		if (val.IsNull()) {
			dpiData_setNull(&target);
			continue;
		}
		switch (type.id()) {
		case LogicalTypeId::BOOLEAN:
			dpiData_setInt64(&target, BooleanValue::Get(val) ? 1 : 0);
			break;
		case LogicalTypeId::UBIGINT:
			dpiData_setUint64(&target, val.GetValue<uint64_t>());
			break;
		case LogicalTypeId::TINYINT:
		case LogicalTypeId::SMALLINT:
		case LogicalTypeId::INTEGER:
		case LogicalTypeId::BIGINT:
		case LogicalTypeId::UTINYINT:
		case LogicalTypeId::USMALLINT:
		case LogicalTypeId::UINTEGER:
			dpiData_setInt64(&target, val.GetValue<int64_t>());
			break;
		case LogicalTypeId::FLOAT:
		case LogicalTypeId::DOUBLE:
		case LogicalTypeId::DECIMAL:
		case LogicalTypeId::HUGEINT:
		case LogicalTypeId::UHUGEINT: {
			if ((type.id() == LogicalTypeId::FLOAT || type.id() == LogicalTypeId::DOUBLE) &&
			    !std::isfinite(val.GetValue<double>())) {
				return false;
			}
			auto text = val.ToString();
			if (dpiVar_setFromBytes(var, (uint32_t)row, text.data(), (uint32_t)text.size()) < 0) {
				ThrowDpiError("number bind");
			}
			break;
		}
		case LogicalTypeId::VARCHAR:
		case LogicalTypeId::UUID:
		case LogicalTypeId::BLOB: {
			auto &text = texts[row];
			if (text.empty()) {
				dpiData_setNull(&target);
			} else if (dpiVar_setFromBytes(var, (uint32_t)row, text.data(), (uint32_t)text.size()) < 0) {
				ThrowDpiError("text bind");
			}
			break;
		}
		case LogicalTypeId::DATE: {
			auto date = DateValue::Get(val);
			if (!date.IsFinite()) {
				return false;
			}
			int32_t y, mo, day;
			Date::Convert(date, y, mo, day);
			dpiData_setTimestamp(&target, (int16_t)y, (uint8_t)mo, (uint8_t)day, 0, 0, 0, 0, 0, 0);
			break;
		}
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_TZ: {
			auto ts = val.GetValueUnsafe<timestamp_t>();
			if (!ts.IsFinite()) {
				return false;
			}
			// a TIMESTAMP WITH TIME ZONE holds a UTC instant
			SetTimestamp(target, ts, 0);
			break;
		}
		case LogicalTypeId::INTERVAL: {
			auto interval = val.GetValueUnsafe<interval_t>();
			if (interval.months != 0) {
				throw InvalidInputException(
				    "An INTERVAL with a month component cannot be stored in an Oracle INTERVAL DAY TO SECOND");
			}
			static constexpr int64_t MICROS_PER_DAY = 86400000000LL;
			int64_t total = static_cast<int64_t>(interval.days) * MICROS_PER_DAY + interval.micros;
			int32_t sign = total < 0 ? -1 : 1;
			int64_t abs_total = total < 0 ? -total : total;
			int64_t rest = abs_total % MICROS_PER_DAY;
			dpiData_setIntervalDS(&target, sign * (int32_t)(abs_total / MICROS_PER_DAY),
			                      sign * (int32_t)(rest / 3600000000LL), sign * (int32_t)(rest / 60000000LL % 60),
			                      sign * (int32_t)(rest / 1000000LL % 60), sign * (int32_t)(rest % 1000000LL * 1000));
			break;
		}
		default:
			return false;
		}
	}
	return true;
}

} // namespace

bool OracleConnection::ExecuteBatch(optional_ptr<ClientContext> context, const string &query,
                                    const vector<LogicalType> &types, idx_t row_count,
                                    const std::function<Value(idx_t, idx_t)> &get_value) {
	if (row_count == 0) {
		return true;
	}
	lock_guard<mutex> guard(connection->connection_lock);
	auto conn = GetConn();

	BatchVars vars;
	vector<Value> values(row_count);
	for (idx_t c = 0; c < types.size(); c++) {
		for (idx_t row = 0; row < row_count; row++) {
			values[row] = get_value(c, row);
		}
		dpiVar *var = nullptr;
		dpiData *data = nullptr;
		bool bound = BindColumn(conn, types[c], values, var, data);
		if (var) {
			vars.vars.push_back(var);
		}
		if (!bound) {
			return false;
		}
	}

	if (DebugPrintQueries()) {
		Printer::Print(StringUtil::Format("%s  -- %llu rows batched\n", query, (unsigned long long)row_count));
	}

	dpiStmt *stmt = nullptr;
	if (dpiConn_prepareStmt(conn, 0, query.c_str(), (uint32_t)query.size(), nullptr, 0, &stmt) < 0) {
		ThrowDpiError("prepare");
	}
	for (idx_t c = 0; c < vars.vars.size(); c++) {
		if (dpiStmt_bindByPos(stmt, (uint32_t)(c + 1), vars.vars[c]) < 0) {
			dpiErrorInfo error_info;
			dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
			dpiStmt_release(stmt);
			throw IOException("Oracle bind failed for query \"%s\": %s", query, string(error_info.message));
		}
	}
	if (dpiStmt_executeMany(stmt, DPI_MODE_EXEC_DEFAULT, (uint32_t)row_count) < 0) {
		dpiErrorInfo error_info;
		dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
		dpiStmt_release(stmt);
		throw IOException("Oracle execute failed for query \"%s\": %s", query, string(error_info.message));
	}
	dpiStmt_release(stmt);
	return true;
}

// ---------------------------------------------------------------------------
// Bind-parameter overloads (named parameters via dpiStmt_bindValueByName)
// ---------------------------------------------------------------------------

unique_ptr<OracleResult> OracleConnection::TryQuery(optional_ptr<ClientContext> context,
                                                     const string &query,
                                                     const unordered_map<string, string> &binds,
                                                     optional_ptr<string> error_message) {
	lock_guard<mutex> guard(connection->connection_lock);
	if (DebugPrintQueries()) {
		string debug_line = query;
		if (!binds.empty()) {
			debug_line += "  -- binds:";
			for (const auto &kv : binds) {
				debug_line += " :" + kv.first + "='" + kv.second + "'";
			}
		}
		Printer::Print(debug_line + "\n");
	}
	try {
		// Data dictionary queries have narrow rows and can return tens of thousands of them
		static constexpr uint32_t METADATA_FETCH_ARRAY_SIZE = 2000;
		auto start = std::chrono::steady_clock::now();
		auto result = ExecuteQuery(query, binds, METADATA_FETCH_ARRAY_SIZE);
		if (DebugPrintQueries()) {
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			    std::chrono::steady_clock::now() - start);
			Printer::Print(StringUtil::Format("-- %llu rows in %llu ms\n", (unsigned long long)result->Count(),
			                                  (unsigned long long)elapsed.count()));
		}
		return result;
	} catch (std::exception &ex) {
		if (error_message) {
			*error_message = ex.what();
		}
		return nullptr;
	}
}

unique_ptr<OracleResult> OracleConnection::Query(optional_ptr<ClientContext> context,
                                                   const string &query,
                                                   const unordered_map<string, string> &binds) {
	string error_msg;
	auto result = TryQuery(context, query, binds, &error_msg);
	if (!result) {
		throw std::runtime_error(error_msg);
	}
	return result;
}

void OracleConnection::Commit() {
	if (!connection || !connection->connection) {
		return;
	}
	dpiErrorInfo error_info;
	if (dpiConn_commit(connection->connection) < 0) {
		dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
		throw IOException("Oracle COMMIT failed: %s", string(error_info.message));
	}
}

void OracleConnection::Rollback() {
	if (!connection || !connection->connection) {
		return;
	}
	dpiConn_rollback(connection->connection); // best-effort
}

OracleVersion OracleConnection::GetOracleVersion() {
	return OracleUtils::GetVersion(GetConn());
}

bool OracleConnection::IsConnectionOk() {
	if (!connection || !connection->connection) {
		return false;
	}
	// Try a cheap round-trip to verify the connection
	dpiStmt *stmt = nullptr;
	if (dpiConn_prepareStmt(connection->connection, 0, "SELECT 1 FROM DUAL", 18,
	                         nullptr, 0, &stmt) < 0) {
		return false;
	}
	uint32_t num_cols = 0;
	bool ok = (dpiStmt_execute(stmt, DPI_MODE_EXEC_DEFAULT, &num_cols) >= 0);
	dpiStmt_release(stmt);
	return ok;
}

bool OracleConnection::IsOpen() {
	return connection.get() != nullptr;
}

void OracleConnection::Close() {
	if (!IsOpen()) {
		return;
	}
	connection = nullptr;
}

void OracleConnection::DebugSetPrintQueries(bool print) {
	debug_oracle_print_queries = print;
}

bool OracleConnection::DebugPrintQueries() {
	return debug_oracle_print_queries;
}

} // namespace duckdb
