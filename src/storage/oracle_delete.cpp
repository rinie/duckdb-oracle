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
	auto &oracle_table = table.Cast<OracleTableEntry>();
	auto &transaction = OracleTransaction::Get(context.client, oracle_table.catalog);

	// The last column in the chunk is the BIGINT rowid index into the transaction's
	// rowid_registry (populated during the materialized scan).
	idx_t rowid_col_idx = chunk.ColumnCount() - 1;
	for (idx_t i = 0; i < chunk.size(); i++) {
		auto val = chunk.GetValue(rowid_col_idx, i);
		if (!val.IsNull()) {
			int64_t rowid_idx = val.GetValue<int64_t>();
			gstate.rowids.push_back(transaction.LookupRowid(rowid_idx));
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
			             OracleUtils::QuoteIdentifier(oracle_table.name.GetIdentifierName()) +
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
	result["Table Name"] = table.name.GetIdentifierName();
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
