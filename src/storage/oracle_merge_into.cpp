#include "storage/oracle_catalog.hpp"
#include "duckdb/execution/operator/persistent/physical_merge_into.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"

namespace duckdb {

PhysicalOperator &OracleCatalog::PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner,
                                               LogicalMergeInto &op, PhysicalOperator &plan) {
	if (op.return_chunk) {
		throw BinderException("RETURNING clause not yet supported for Oracle MERGE");
	}
	// The target rows are read while the actions modify them: fetch the scans completely first
	MaterializeOracleScans(plan);
	// DuckDB plans every action through our regular INSERT/UPDATE/DELETE planning and projects the rows of the
	// merge into the layout those expect ([new values..., row id] for UPDATE, trailing row id for DELETE)
	auto &merge = Catalog::PlanMergeInto(context, planner, op, plan);
	// The Oracle sinks are not parallel, so the actions must not be fed concurrently
	merge.Cast<PhysicalMergeInto>().parallel = false;
	return merge;
}

} // namespace duckdb
