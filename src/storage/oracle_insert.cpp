#include "storage/oracle_insert.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_transaction.hpp"
#include "storage/oracle_table_entry.hpp"
#include "oracle_scanner.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

OracleInsert::OracleInsert(PhysicalPlan &physical_plan, LogicalOperator &op,
                             TableCatalogEntry &table,
                             physical_index_vector_t<idx_t> column_index_map_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, 1),
      table(&table), schema(nullptr), column_index_map(std::move(column_index_map_p)) {
}

OracleInsert::OracleInsert(PhysicalPlan &physical_plan, LogicalOperator &op,
                             SchemaCatalogEntry &schema,
                             unique_ptr<BoundCreateTableInfo> info_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, 1),
      table(nullptr), schema(&schema), info(std::move(info_p)) {
}

// ---------------------------------------------------------------------------
// Sink state
// ---------------------------------------------------------------------------

class OracleInsertGlobalState : public GlobalSinkState {
public:
	explicit OracleInsertGlobalState(OracleTableEntry &table)
	    : table(table), insert_count(0) {
	}

	OracleTableEntry &table;
	idx_t insert_count;
	vector<string> insert_column_names;

	// Build INSERT INTO ... VALUES (...) statement
	string BuildInsertSQL(DataChunk &chunk) const;
};

string OracleInsertGlobalState::BuildInsertSQL(DataChunk &chunk) const {
	auto &col_names = insert_column_names;
	auto &tbl = table;
	// Build: INSERT ALL INTO t(c1,c2) VALUES(v1,v2) ... SELECT * FROM DUAL
	// For small batches we use INSERT ALL; for simplicity use one row at a time
	string base = "INSERT INTO " +
	              OracleUtils::QuoteIdentifier(tbl.oracle_schema_name) + "." +
	              OracleUtils::QuoteIdentifier(tbl.name) + "(";
	for (idx_t c = 0; c < col_names.size(); c++) {
		if (c > 0) base += ", ";
		base += OracleUtils::QuoteIdentifier(col_names[c]);
	}
	base += ") VALUES(";
	return base;
}

unique_ptr<GlobalSinkState> OracleInsert::GetGlobalSinkState(ClientContext &context) const {
	optional_ptr<OracleTableEntry> insert_table;
	if (!table) {
		auto &schema_ref = *schema.get_mutable();
		insert_table = &schema_ref.CreateTable(schema_ref.GetCatalogTransaction(context),
		                                        *info)->Cast<OracleTableEntry>();
	} else {
		insert_table = &table.get_mutable()->Cast<OracleTableEntry>();
	}

	auto result = make_uniq<OracleInsertGlobalState>(*insert_table);
	auto &columns = insert_table->GetColumns();

	if (!column_index_map.empty()) {
		vector<PhysicalIndex> col_indexes;
		col_indexes.resize(columns.LogicalColumnCount(),
		                    PhysicalIndex(DConstants::INVALID_INDEX));
		for (idx_t c = 0; c < column_index_map.size(); c++) {
			auto mapped = column_index_map[PhysicalIndex(c)];
			if (mapped != DConstants::INVALID_INDEX) {
				col_indexes[mapped] = PhysicalIndex(c);
			}
		}
		for (auto &idx : col_indexes) {
			if (idx.index != DConstants::INVALID_INDEX) {
				result->insert_column_names.push_back(
				    columns.GetColumn(idx).GetName());
			}
		}
	} else {
		// All columns
		for (auto &col : columns.Logical()) {
			result->insert_column_names.push_back(col.GetName());
		}
	}
	return std::move(result);
}

// ---------------------------------------------------------------------------
// Sink
// ---------------------------------------------------------------------------

static string ValueToOracleSQL(const Value &val) {
	if (val.IsNull()) {
		return "NULL";
	}
	switch (val.type().id()) {
	case LogicalTypeId::BOOLEAN:
		return BooleanValue::Get(val) ? "1" : "0";
	case LogicalTypeId::VARCHAR:
		return OracleUtils::WriteLiteral(StringValue::Get(val));
	case LogicalTypeId::DATE: {
		auto d = DateValue::Get(val);
		int32_t y, m, day;
		Date::Convert(d, y, m, day);
		return StringUtil::Format("DATE '%04d-%02d-%02d'", y, m, day);
	}
	case LogicalTypeId::TIMESTAMP: {
		auto ts = TimestampValue::Get(val);
		auto d = Timestamp::GetDate(ts);
		auto t = Timestamp::GetTime(ts);
		int32_t y, mo, day, h, mi, s, us;
		Date::Convert(d, y, mo, day);
		Time::Convert(t, h, mi, s, us);
		return StringUtil::Format(
		    "TIMESTAMP '%04d-%02d-%02d %02d:%02d:%02d.%06d'", y, mo, day, h, mi, s, us);
	}
	case LogicalTypeId::BLOB: {
		// Hex-encode raw bytes for Oracle HEXTORAW(...)
		auto raw = StringValue::Get(val);
		string hex;
		static const char hx[] = "0123456789ABCDEF";
		for (uint8_t b : raw) {
			hex += hx[b >> 4];
			hex += hx[b & 0xF];
		}
		return "HEXTORAW('" + hex + "')";
	}
	default:
		return OracleUtils::WriteLiteral(val.ToString());
	}
}

SinkResultType OracleInsert::Sink(ExecutionContext &context, DataChunk &chunk,
                                   OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<OracleInsertGlobalState>();
	auto &transaction = OracleTransaction::Get(context.client, gstate.table.catalog);
	auto &connection = transaction.GetConnection();

	auto base_sql = gstate.BuildInsertSQL(chunk);
	idx_t num_cols = gstate.insert_column_names.size();

	for (idx_t row = 0; row < chunk.size(); row++) {
		string sql = base_sql;
		for (idx_t c = 0; c < num_cols; c++) {
			if (c > 0) sql += ", ";
			auto val = chunk.GetValue(c, row);
			sql += ValueToOracleSQL(val);
		}
		sql += ")";
		connection.Execute(context.client, sql);
	}
	gstate.insert_count += chunk.size();
	return SinkResultType::NEED_MORE_INPUT;
}

SinkFinalizeType OracleInsert::Finalize(Pipeline &pipeline, Event &event,
                                         ClientContext &context,
                                         OperatorSinkFinalizeInput &input) const {
	// Transaction will be committed by the transaction manager
	return SinkFinalizeType::READY;
}

// ---------------------------------------------------------------------------
// Source (return count)
// ---------------------------------------------------------------------------

SourceResultType OracleInsert::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                OperatorSourceInput &input) const {
	auto &insert_gstate = sink_state->Cast<OracleInsertGlobalState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(insert_gstate.insert_count));
	return SourceResultType::FINISHED;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

string OracleInsert::GetName() const {
	return table ? "ORA_INSERT" : "ORA_CREATE_TABLE_AS";
}

InsertionOrderPreservingMap<string> OracleInsert::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table Name"] = table ? table->name : info->Base().table;
	return result;
}

// ---------------------------------------------------------------------------
// Plan (called from OracleCatalog::PlanInsert / PlanCreateTableAs)
// ---------------------------------------------------------------------------

static PhysicalOperator &AddCastToOracleTypes(ClientContext &context,
                                               PhysicalPlanGenerator &planner,
                                               PhysicalOperator &plan) {
	bool require_cast = false;
	auto &child_types = plan.GetTypes();
	for (auto &type : child_types) {
		auto oracle_type = OracleUtils::ToOracleType(type);
		if (oracle_type != type) {
			require_cast = true;
			break;
		}
	}
	if (!require_cast) {
		return plan;
	}
	vector<LogicalType> oracle_types;
	vector<unique_ptr<Expression>> select_list;
	for (idx_t i = 0; i < child_types.size(); i++) {
		auto &type = child_types[i];
		unique_ptr<Expression> expr = make_uniq<BoundReferenceExpression>(type, i);
		auto oracle_type = OracleUtils::ToOracleType(type);
		if (oracle_type != type) {
			expr = BoundCastExpression::AddCastToType(context, std::move(expr), oracle_type);
		}
		oracle_types.push_back(std::move(oracle_type));
		select_list.push_back(std::move(expr));
	}
	auto &proj = planner.Make<PhysicalProjection>(std::move(oracle_types),
	                                               std::move(select_list),
	                                               plan.estimated_cardinality);
	proj.children.push_back(plan);
	return proj;
}

PhysicalOperator &OracleCatalog::PlanInsert(ClientContext &context,
                                             PhysicalPlanGenerator &planner,
                                             LogicalInsert &op,
                                             optional_ptr<PhysicalOperator> plan) {
	if (op.return_chunk) {
		throw BinderException(
		    "RETURNING clause not yet supported for Oracle table insertion");
	}
	if (op.on_conflict_info.action_type != OnConflictAction::THROW) {
		throw BinderException("ON CONFLICT not yet supported for Oracle table insertion");
	}
	D_ASSERT(plan);
	MaterializeOracleScans(*plan);
	auto &inner_plan = AddCastToOracleTypes(context, planner, *plan);
	auto &insert = planner.Make<OracleInsert>(op, op.table, op.column_index_map);
	insert.children.push_back(inner_plan);
	return insert;
}

PhysicalOperator &OracleCatalog::PlanCreateTableAs(ClientContext &context,
                                                     PhysicalPlanGenerator &planner,
                                                     LogicalCreateTable &op,
                                                     PhysicalOperator &plan) {
	auto &inner_plan = AddCastToOracleTypes(context, planner, plan);
	MaterializeOracleScans(inner_plan);
	auto &insert = planner.Make<OracleInsert>(op, op.schema, std::move(op.info));
	insert.children.push_back(inner_plan);
	return insert;
}

} // namespace duckdb
