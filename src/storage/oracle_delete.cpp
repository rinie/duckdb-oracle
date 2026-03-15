#include "storage/oracle_delete.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_transaction.hpp"
#include "storage/oracle_table_entry.hpp"
#include "oracle_scanner.hpp"

namespace duckdb {

OracleDelete::OracleDelete(PhysicalPlan &physical_plan, LogicalOperator &op,
                             TableCatalogEntry &table)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, 1),
      table(table) {
}

class OracleDeleteGlobalState : public GlobalSinkState {
public:
	explicit OracleDeleteGlobalState() : deleted_count(0) {
	}
	idx_t deleted_count;
	// rowids collected for DELETE
	vector<string> rowids;
};

unique_ptr<GlobalSinkState> OracleDelete::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<OracleDeleteGlobalState>();
}

SinkResultType OracleDelete::Sink(ExecutionContext &context, DataChunk &chunk,
                                   OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<OracleDeleteGlobalState>();
	// Collect ROWIDs from the ROWID column (last column in the chunk)
	// This requires the scan to emit ctid (ROWID in Oracle) - see oracle_scanner
	// For simplicity, delete the entire table row by rowid from the scan's
	// emitted values. DuckDB passes the __rowid column as a VARCHAR ROWID.
	auto &rowid_col = chunk.data[chunk.ColumnCount() - 1];
	for (idx_t i = 0; i < chunk.size(); i++) {
		auto val = chunk.GetValue(chunk.ColumnCount() - 1, i);
		if (!val.IsNull()) {
			gstate.rowids.push_back(StringValue::Get(val));
		}
	}
	gstate.deleted_count += chunk.size();
	return SinkResultType::NEED_MORE_INPUT;
}

SinkFinalizeType OracleDelete::Finalize(Pipeline &pipeline, Event &event,
                                         ClientContext &context,
                                         OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<OracleDeleteGlobalState>();
	auto &oracle_table = table.Cast<OracleTableEntry>();
	auto &transaction = OracleTransaction::Get(context, oracle_table.catalog);
	auto &connection = transaction.GetConnection();

	// DELETE using collected ROWIDs
	if (!gstate.rowids.empty()) {
		for (auto &rowid : gstate.rowids) {
			string sql = "DELETE FROM " +
			             OracleUtils::QuoteIdentifier(oracle_table.oracle_schema_name) + "." +
			             OracleUtils::QuoteIdentifier(oracle_table.name) +
			             " WHERE ROWID = " + OracleUtils::WriteLiteral(rowid);
			connection.Execute(context, sql);
		}
	}
	return SinkFinalizeType::READY;
}

SourceResultType OracleDelete::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<OracleDeleteGlobalState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(gstate.deleted_count));
	return SourceResultType::FINISHED;
}

string OracleDelete::GetName() const {
	return "ORA_DELETE";
}

InsertionOrderPreservingMap<string> OracleDelete::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table Name"] = table.name;
	return result;
}

PhysicalOperator &OracleCatalog::PlanDelete(ClientContext &context,
                                              PhysicalPlanGenerator &planner,
                                              LogicalDelete &op,
                                              PhysicalOperator &plan) {
	if (op.return_chunk) {
		throw BinderException("RETURNING clause not yet supported for Oracle DELETE");
	}
	MaterializeOracleScans(plan);
	auto &del = planner.Make<OracleDelete>(op, op.table);
	del.children.push_back(plan);
	return del;
}

} // namespace duckdb
