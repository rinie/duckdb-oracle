#include "duckdb.hpp"
#include "oracle_scanner.hpp"
#include "oracle_conversion.hpp"
#include <dpi.h>

namespace duckdb {

struct OracleQueryBindData : public TableFunctionData {
	string dsn;
	string query;
	vector<LogicalType> types;
	vector<OracleType> oracle_types;
};

struct OracleQueryGlobalState : public GlobalTableFunctionState {
	~OracleQueryGlobalState() override {
		if (stmt) {
			dpiStmt_release(stmt);
		}
	}

	OracleConnection connection;
	dpiStmt *stmt = nullptr;
	bool done = false;
};

//! Data dictionary style name of a type, as understood by OracleUtils::TypeToLogicalType
static string OracleTypeName(dpiOracleTypeNum type) {
	switch (type) {
	case DPI_ORACLE_TYPE_VARCHAR:
		return "VARCHAR2";
	case DPI_ORACLE_TYPE_NVARCHAR:
		return "NVARCHAR2";
	case DPI_ORACLE_TYPE_CHAR:
		return "CHAR";
	case DPI_ORACLE_TYPE_NCHAR:
		return "NCHAR";
	case DPI_ORACLE_TYPE_ROWID:
		return "ROWID";
	case DPI_ORACLE_TYPE_UROWID:
		return "UROWID";
	case DPI_ORACLE_TYPE_RAW:
		return "RAW";
	case DPI_ORACLE_TYPE_NATIVE_FLOAT:
		return "BINARY_FLOAT";
	case DPI_ORACLE_TYPE_NATIVE_DOUBLE:
		return "BINARY_DOUBLE";
	case DPI_ORACLE_TYPE_NATIVE_INT:
	case DPI_ORACLE_TYPE_NATIVE_UINT:
	case DPI_ORACLE_TYPE_NUMBER:
		return "NUMBER";
	case DPI_ORACLE_TYPE_DATE:
		return "DATE";
	case DPI_ORACLE_TYPE_TIMESTAMP:
		return "TIMESTAMP";
	case DPI_ORACLE_TYPE_TIMESTAMP_TZ:
		return "TIMESTAMP WITH TIME ZONE";
	case DPI_ORACLE_TYPE_TIMESTAMP_LTZ:
		return "TIMESTAMP WITH LOCAL TIME ZONE";
	case DPI_ORACLE_TYPE_INTERVAL_DS:
		return "INTERVAL DAY TO SECOND";
	case DPI_ORACLE_TYPE_INTERVAL_YM:
		return "INTERVAL YEAR TO MONTH";
	case DPI_ORACLE_TYPE_CLOB:
		return "CLOB";
	case DPI_ORACLE_TYPE_NCLOB:
		return "NCLOB";
	case DPI_ORACLE_TYPE_BLOB:
		return "BLOB";
	case DPI_ORACLE_TYPE_LONG_VARCHAR:
	case DPI_ORACLE_TYPE_LONG_NVARCHAR:
		return "LONG";
	case DPI_ORACLE_TYPE_LONG_RAW:
		return "LONG RAW";
	case DPI_ORACLE_TYPE_XMLTYPE:
		return "XMLTYPE";
	case DPI_ORACLE_TYPE_BOOLEAN:
		return "BOOLEAN";
	default:
		return "UNKNOWN";
	}
}

[[noreturn]] static void ThrowOracleError(const string &what, const string &query) {
	dpiErrorInfo error_info;
	dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
	throw IOException("Oracle %s failed for query \"%s\": %s", what, query, string(error_info.message));
}

//! The statement text without trailing whitespace and semicolons, which Oracle rejects
static string CleanQuery(string query) {
	StringUtil::Trim(query);
	while (!query.empty() && query.back() == ';') {
		query.pop_back();
		StringUtil::Trim(query);
	}
	return query;
}

static unique_ptr<FunctionData> OracleQueryBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<OracleQueryBindData>();
	result->dsn = input.inputs[0].GetValue<string>();
	result->query = CleanQuery(input.inputs[1].GetValue<string>());

	// Describe the columns without running the statement
	auto con = OracleConnection::Open(result->dsn, result->dsn);
	dpiStmt *stmt = nullptr;
	if (dpiConn_prepareStmt(con.GetConn(), 0, result->query.c_str(), (uint32_t)result->query.size(), nullptr, 0,
	                        &stmt) < 0) {
		ThrowOracleError("prepare", result->query);
	}
	uint32_t num_cols = 0;
	if (dpiStmt_execute(stmt, DPI_MODE_EXEC_DESCRIBE_ONLY, &num_cols) < 0) {
		try {
			ThrowOracleError("describe", result->query);
		} catch (...) {
			dpiStmt_release(stmt);
			throw;
		}
	}
	if (num_cols == 0) {
		dpiStmt_release(stmt);
		throw InvalidInputException("oracle_query only runs queries that return columns; use oracle_execute for "
		                            "INSERT, UPDATE, DELETE and DDL statements");
	}

	case_insensitive_set_t used_names;
	for (uint32_t i = 1; i <= num_cols; i++) {
		dpiQueryInfo info;
		if (dpiStmt_getQueryInfo(stmt, i, &info) < 0) {
			dpiStmt_release(stmt);
			ThrowOracleError("column description", result->query);
		}
		string name(info.name, info.nameLength);
		auto unique_name = name;
		for (idx_t suffix = 1; used_names.count(unique_name) > 0; suffix++) {
			unique_name = name + "_" + to_string(suffix);
		}
		used_names.insert(unique_name);

		OracleTypeData type_data;
		type_data.type_name = OracleTypeName(info.typeInfo.oracleTypeNum);
		type_data.data_precision = info.typeInfo.precision;
		type_data.data_scale = info.typeInfo.scale;
		OracleType oracle_type;
		auto type = OracleUtils::TypeToLogicalType(type_data, oracle_type);

		names.emplace_back(Identifier(unique_name));
		return_types.push_back(type);
		result->types.push_back(type);
		result->oracle_types.push_back(oracle_type);
	}
	dpiStmt_release(stmt);
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> OracleQueryInitGlobal(ClientContext &context,
                                                                  TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<OracleQueryBindData>();
	auto gstate = make_uniq<OracleQueryGlobalState>();
	gstate->connection = OracleConnection::Open(bind_data.dsn, bind_data.dsn);

	if (OracleConnection::DebugPrintQueries()) {
		Printer::Print(bind_data.query + "\n");
	}
	auto conn = gstate->connection.GetConn();
	if (dpiConn_prepareStmt(conn, 0, bind_data.query.c_str(), (uint32_t)bind_data.query.size(), nullptr, 0,
	                        &gstate->stmt) < 0) {
		ThrowOracleError("prepare", bind_data.query);
	}
	dpiStmt_setFetchArraySize(gstate->stmt, OracleBindData::DEFAULT_FETCH_ROWS);
	uint32_t num_cols = 0;
	if (dpiStmt_execute(gstate->stmt, DPI_MODE_EXEC_DEFAULT, &num_cols) < 0) {
		ThrowOracleError("execute", bind_data.query);
	}
	// CLOB/BLOB columns are fetched as text/bytes instead of LOB locators
	OracleUtils::DefineLobsAsBytes(gstate->stmt, num_cols);
	// ROWID columns are fetched as text
	for (uint32_t i = 0; i < num_cols; i++) {
		dpiQueryInfo info;
		if (dpiStmt_getQueryInfo(gstate->stmt, i + 1, &info) == 0 &&
		    (info.typeInfo.oracleTypeNum == DPI_ORACLE_TYPE_ROWID ||
		     info.typeInfo.oracleTypeNum == DPI_ORACLE_TYPE_UROWID)) {
			dpiStmt_defineValue(gstate->stmt, i + 1, DPI_ORACLE_TYPE_VARCHAR, DPI_NATIVE_TYPE_BYTES, 4000, 1,
			                    nullptr);
		}
	}
	return std::move(gstate);
}

static void OracleQueryScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<OracleQueryBindData>();
	auto &gstate = data.global_state->Cast<OracleQueryGlobalState>();

	idx_t count = 0;
	while (!gstate.done && count < STANDARD_VECTOR_SIZE) {
		int found = 0;
		uint32_t buffer_row_index = 0;
		if (dpiStmt_fetch(gstate.stmt, &found, &buffer_row_index) < 0) {
			ThrowOracleError("fetch", bind_data.query);
		}
		if (!found) {
			gstate.done = true;
			break;
		}
		for (idx_t c = 0; c < output.ColumnCount(); c++) {
			dpiNativeTypeNum native_type;
			dpiData *value;
			if (dpiStmt_getQueryValue(gstate.stmt, (uint32_t)(c + 1), &native_type, &value) < 0) {
				ThrowOracleError("getQueryValue", bind_data.query);
			}
			OracleConvertValue(output.data[c], count, value, native_type, bind_data.types[c],
			                   bind_data.oracle_types[c]);
		}
		count++;
	}
	output.SetCardinality(count);
}

OracleQueryFunction::OracleQueryFunction()
    : TableFunction("oracle_query", {LogicalType::VARCHAR, LogicalType::VARCHAR}, OracleQueryScan, OracleQueryBind,
                    OracleQueryInitGlobal) {
}

} // namespace duckdb
