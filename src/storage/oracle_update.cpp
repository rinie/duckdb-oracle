#include "storage/oracle_update.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_transaction.hpp"
#include "storage/oracle_table_entry.hpp"
#include "oracle_scanner.hpp"

namespace duckdb {

OracleUpdate::OracleUpdate(PhysicalPlan &physical_plan, LogicalOperator &op,
                             TableCatalogEntry &table, vector<PhysicalIndex> columns_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, 1),
      table(table), columns(std::move(columns_p)) {
}

class OracleUpdateGlobalState : public GlobalSinkState {
public:
	explicit OracleUpdateGlobalState() : update_count(0) {
	}
	idx_t update_count;
};

unique_ptr<GlobalSinkState> OracleUpdate::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<OracleUpdateGlobalState>();
}

SinkResultType OracleUpdate::Sink(ExecutionContext &context, DataChunk &chunk,
                                   OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<OracleUpdateGlobalState>();
	auto &oracle_table = table.Cast<OracleTableEntry>();
	auto &transaction = OracleTransaction::Get(context.client, oracle_table.catalog);
	auto &connection = transaction.GetConnection();

	// chunk layout: [updated_col1, updated_col2, ..., ROWID]
	// ROWID is the last column
	idx_t ncols = columns.size();

	// one array-bound statement per chunk; rows without a ROWID match nothing
	{
		string batch_sql = "UPDATE " + OracleUtils::QuoteIdentifier(oracle_table.oracle_schema_name) + "." +
		                   OracleUtils::QuoteIdentifier(oracle_table.name.GetIdentifierName()) + " SET ";
		vector<LogicalType> types;
		for (idx_t c = 0; c < ncols; c++) {
			auto &col = oracle_table.GetColumns().GetColumn(columns[c]);
			if (c > 0) batch_sql += ", ";
			batch_sql += OracleUtils::QuoteIdentifier(col.Name().GetIdentifierName()) + " = :" + std::to_string(c + 1);
			types.push_back(chunk.data[c].GetType());
		}
		batch_sql += " WHERE ROWID = :" + std::to_string(ncols + 1);
		types.push_back(LogicalType::VARCHAR);
		if (connection.ExecuteBatch(context.client, batch_sql, types, chunk.size(), [&](idx_t c, idx_t row) {
			    if (c < ncols) {
				    return chunk.GetValue(c, row);
			    }
			    auto rowid_val = chunk.GetValue(ncols, row);
			    if (rowid_val.IsNull()) {
				    return Value(LogicalType::VARCHAR);
			    }
			    return Value(transaction.LookupRowid(rowid_val.GetValue<int64_t>()));
		    })) {
			gstate.update_count += chunk.size();
			return SinkResultType::NEED_MORE_INPUT;
		}
	}

	for (idx_t row = 0; row < chunk.size(); row++) {
		auto rowid_val = chunk.GetValue(ncols, row); // ROWID index is after the updated cols
		if (rowid_val.IsNull()) {
			continue;
		}
		int64_t rowid_idx = rowid_val.GetValue<int64_t>();
		const auto &rowid = transaction.LookupRowid(rowid_idx);

		string sql = "UPDATE " +
		             OracleUtils::QuoteIdentifier(oracle_table.oracle_schema_name) + "." +
		             OracleUtils::QuoteIdentifier(oracle_table.name.GetIdentifierName()) + " SET ";
		for (idx_t c = 0; c < ncols; c++) {
			if (c > 0) sql += ", ";
			auto &col = oracle_table.GetColumns().GetColumn(columns[c]);
			sql += OracleUtils::QuoteIdentifier(col.Name().GetIdentifierName()) + " = ";
			auto val = chunk.GetValue(c, row);
			sql += OracleUtils::ValueToOracleSQL(val);
		}
		sql += " WHERE ROWID = " + OracleUtils::WriteLiteral(rowid);
		connection.Execute(context.client, sql);
	}
	gstate.update_count += chunk.size();
	return SinkResultType::NEED_MORE_INPUT;
}

SinkFinalizeType OracleUpdate::Finalize(Pipeline &pipeline, Event &event,
                                         ClientContext &context,
                                         OperatorSinkFinalizeInput &input) const {
	return SinkFinalizeType::READY;
}

SourceResultType OracleUpdate::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<OracleUpdateGlobalState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(gstate.update_count));
	return SourceResultType::FINISHED;
}

string OracleUpdate::GetName() const {
	return "ORA_UPDATE";
}

InsertionOrderPreservingMap<string> OracleUpdate::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table Name"] = table.name.GetIdentifierName();
	return result;
}

PhysicalOperator &OracleCatalog::PlanUpdate(ClientContext &context,
                                              PhysicalPlanGenerator &planner,
                                              LogicalUpdate &op,
                                              PhysicalOperator &plan) {
	if (op.return_chunk) {
		throw BinderException("RETURNING clause not yet supported for Oracle UPDATE");
	}
	MaterializeOracleScans(plan);
	auto &upd = planner.Make<OracleUpdate>(op, op.table, op.columns);
	upd.children.push_back(plan);
	return upd;
}

} // namespace duckdb
