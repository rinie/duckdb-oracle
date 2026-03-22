//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/oracle_insert.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"

namespace duckdb {
class OracleTableEntry;

class OracleInsert : public PhysicalOperator {
public:
	//! INSERT INTO
	OracleInsert(PhysicalPlan &physical_plan, LogicalOperator &op,
	              TableCatalogEntry &table,
	              physical_index_vector_t<idx_t> column_index_map);
	//! CREATE TABLE AS
	OracleInsert(PhysicalPlan &physical_plan, LogicalOperator &op,
	              SchemaCatalogEntry &schema, unique_ptr<BoundCreateTableInfo> info);

	optional_ptr<TableCatalogEntry> table;
	optional_ptr<SchemaCatalogEntry> schema;
	unique_ptr<BoundCreateTableInfo> info;
	physical_index_vector_t<idx_t> column_index_map;

public:
	// Source interface
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                  OperatorSourceInput &input) const override;
	bool IsSource() const override {
		return true;
	}

public:
	// Sink interface
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk,
	                     OperatorSinkInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                           OperatorSinkFinalizeInput &input) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}

	string GetName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;
};

} // namespace duckdb
