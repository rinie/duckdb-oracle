#include "duckdb.hpp"
#include "oracle_scanner.hpp"
#include "oracle_result.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_transaction.hpp"

namespace duckdb {

struct OracleExecuteBindData : public TableFunctionData {
	string dsn;
	string query;
};

struct OracleExecuteGlobalState : public GlobalTableFunctionState {
	bool finished = false;
};

static unique_ptr<FunctionData> OracleExecuteBind(ClientContext &context,
                                                    TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types,
                                                    vector<Identifier> &names) {
	auto result = make_uniq<OracleExecuteBindData>();
	result->dsn = input.inputs[0].GetValue<string>();
	result->query = input.inputs[1].GetValue<string>();
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("Success");
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState>
OracleExecuteInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<OracleExecuteGlobalState>();
}

static void OracleExecuteScan(ClientContext &context, TableFunctionInput &data,
                               DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<OracleExecuteBindData>();
	auto &gstate = data.global_state->Cast<OracleExecuteGlobalState>();
	if (gstate.finished) {
		return;
	}
	auto con = OracleConnection::Open(bind_data.dsn, bind_data.dsn);
	con.Execute(context, bind_data.query);
	con.Commit();

	output.SetCardinality(1);
	output.SetValue(0, 0, Value("OK"));
	gstate.finished = true;
}

OracleExecuteFunction::OracleExecuteFunction()
    : TableFunction("oracle_execute", {LogicalType::VARCHAR, LogicalType::VARCHAR},
                    OracleExecuteScan, OracleExecuteBind, OracleExecuteInitGlobal) {
}

} // namespace duckdb
