#include "duckdb.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "oracle_scanner.hpp"
#include "oracle_result.hpp"

namespace duckdb {

struct OracleAttachData : public TableFunctionData {
	bool finished = false;
	string source_schema = "";
	string sink_schema = "main";
	string suffix = "";
	bool overwrite = false;
	bool filter_pushdown = false;
	string dsn = "";
};

static unique_ptr<FunctionData> OracleAttachBind(ClientContext &context,
                                                   TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types,
                                                   vector<Identifier> &names) {
	auto result = make_uniq<OracleAttachData>();
	result->dsn = input.inputs[0].GetValue<string>();

	for (auto &kv : input.named_parameters) {
		if (kv.first == "source_schema") {
			result->source_schema = StringValue::Get(kv.second);
		} else if (kv.first == "sink_schema") {
			result->sink_schema = StringValue::Get(kv.second);
		} else if (kv.first == "overwrite") {
			result->overwrite = BooleanValue::Get(kv.second);
		} else if (kv.first == "filter_pushdown") {
			result->filter_pushdown = BooleanValue::Get(kv.second);
		}
	}

	return_types.push_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");
	return std::move(result);
}

static void OracleAttachScan(ClientContext &context, TableFunctionInput &data_p,
                              DataChunk &output) {
	auto &data = (OracleAttachData &)*data_p.bind_data;
	if (data.finished) {
		return;
	}

	auto conn = OracleConnection::Open(data.dsn, data.dsn);

	// Default schema: current Oracle user (uppercase)
	string schema = data.source_schema;
	if (schema.empty()) {
		// Get the current user from Oracle
		auto res = conn.Query(context, "SELECT USER FROM DUAL");
		if (res && res->Count() > 0) {
			schema = res->GetString(0, 0);
		}
	} else {
		schema = StringUtil::Upper(schema);
	}

	// Query tables visible in this schema
	string fetch_tables = StringUtil::Format(
	    "SELECT table_name FROM all_tables WHERE owner = %s ORDER BY table_name",
	    OracleUtils::WriteLiteral(schema));

	auto res = conn.Query(context, fetch_tables);
	auto dconn = Connection(context.db->GetDatabase(context));

	for (idx_t row = 0; row < res->Count(); row++) {
		auto table_name = res->GetString(row, 0);
		string query;
		if (data.overwrite) {
			query = "CREATE OR REPLACE VIEW ";
		} else {
			query = "CREATE VIEW IF NOT EXISTS ";
		}
		if (!data.sink_schema.empty()) {
			query += KeywordHelper::WriteQuoted(data.sink_schema, '"') + ".";
		}
		query += KeywordHelper::WriteQuoted(table_name, '"');
		query += " AS SELECT * FROM ";
		if (data.filter_pushdown) {
			query += "oracle_scan_pushdown";
		} else {
			query += "oracle_scan";
		}
		query += "(";
		query += KeywordHelper::WriteQuoted(data.dsn);
		query += ", ";
		query += KeywordHelper::WriteQuoted(schema);
		query += ", ";
		query += KeywordHelper::WriteQuoted(table_name);
		query += ");";
		dconn.Query(query);
	}

	data.finished = true;
}

static FunctionSignature OracleAttachSignature() {
	FunctionSignature signature;
	signature.AddParameter("connection", LogicalType::VARCHAR)
	    .WithTypedKwargs("options", [](TypedKwargs &options) {
		    options.Add("overwrite", LogicalType::BOOLEAN)
		        .Add("filter_pushdown", LogicalType::BOOLEAN)
		        .Add("source_schema", LogicalType::VARCHAR)
		        .Add("sink_schema", LogicalType::VARCHAR)
		        .Add("suffix", LogicalType::VARCHAR);
	    });
	return signature;
}

OracleAttachFunction::OracleAttachFunction()
    : TableFunction("oracle_attach", OracleAttachSignature(), OracleAttachScan, OracleAttachBind) {
}

} // namespace duckdb
