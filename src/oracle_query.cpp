#include "duckdb.hpp"
#include "oracle_scanner.hpp"
#include "oracle_result.hpp"
#include "oracle_conversion.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_transaction.hpp"
#include <dpi.h>

namespace duckdb {

struct OracleQueryBindData : public TableFunctionData {
	string dsn;
	string query;
	bool finished = false;

	// Output schema, determined after running the query
	vector<string> names;
	vector<LogicalType> types;
	vector<OracleType> oracle_types;
};

struct OracleQueryGlobalState : public GlobalTableFunctionState {
	unique_ptr<OracleResult> result;
	idx_t current_row = 0;
};

static unique_ptr<FunctionData> OracleQueryBind(ClientContext &context,
                                                  TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types,
                                                  vector<Identifier> &names) {
	auto result = make_uniq<OracleQueryBindData>();
	result->dsn = input.inputs[0].GetValue<string>();
	result->query = input.inputs[1].GetValue<string>();

	// Execute query to get schema
	auto con = OracleConnection::Open(result->dsn, result->dsn);
	// We run the query and collect results; for schema-only we could DESCRIBE but
	// simpler to just execute and inspect the result
	auto qresult = con.Query(context, result->query);

	// Infer column types as VARCHAR for simplicity (all values already as strings)
	if (qresult && qresult->Count() > 0) {
		idx_t ncols = qresult->rows[0].size();
		for (idx_t c = 0; c < ncols; c++) {
			result->names.push_back("column" + to_string(c));
			names.emplace_back(Identifier("column" + to_string(c)));
			return_types.push_back(LogicalType::VARCHAR);
			OracleType ot;
			ot.info = OracleTypeAnnotation::CAST_TO_VARCHAR;
			result->oracle_types.push_back(ot);
		}
	}

	result->types = return_types;

	// Store result for output
	// (we materialize it during bind for simplicity)
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState>
OracleQueryInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<OracleQueryBindData>();
	auto gstate = make_uniq<OracleQueryGlobalState>();
	// Re-run query (bind may have already run it, but we need it here for output)
	auto con = OracleConnection::Open(bind_data.dsn, bind_data.dsn);
	gstate->result = con.Query(context, bind_data.query);
	gstate->current_row = 0;
	return std::move(gstate);
}

static void OracleQueryScan(ClientContext &context, TableFunctionInput &data,
                              DataChunk &output) {
	auto &gstate = data.global_state->Cast<OracleQueryGlobalState>();
	auto &res = *gstate.result;

	idx_t count = 0;
	while (gstate.current_row < res.Count() && count < STANDARD_VECTOR_SIZE) {
		for (idx_t c = 0; c < output.ColumnCount(); c++) {
			auto &col = output.data[c];
			if (res.IsNull(gstate.current_row, c)) {
				FlatVector::SetNull(col, count, true);
			} else {
				auto str = res.GetString(gstate.current_row, c);
				FlatVector::GetDataMutable<string_t>(col)[count] = StringVector::AddString(col, str);
			}
		}
		gstate.current_row++;
		count++;
	}
	output.SetCardinality(count);
}

OracleQueryFunction::OracleQueryFunction()
    : TableFunction("oracle_query", {LogicalType::VARCHAR, LogicalType::VARCHAR},
                    OracleQueryScan, OracleQueryBind, OracleQueryInitGlobal) {
}

} // namespace duckdb
