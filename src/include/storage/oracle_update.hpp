//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/oracle_update.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/planner/operator/logical_update.hpp"

namespace duckdb {
class OracleTableEntry;

class OracleUpdate : public PhysicalOperator {
public:
	OracleUpdate(PhysicalPlan &physical_plan, LogicalOperator &op,
	              TableCatalogEntry &table, vector<PhysicalIndex> columns);

	TableCatalogEntry &table;
	vector<PhysicalIndex> columns;

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
