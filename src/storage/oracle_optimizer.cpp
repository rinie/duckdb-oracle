#include "storage/oracle_optimizer.hpp"
#include "storage/oracle_catalog.hpp"
#include "oracle_scanner.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"

namespace duckdb {

struct OracleOperators {
	reference_map_t<OracleCatalog, vector<reference<LogicalGet>>> scans;
};

// Push LIMIT/OFFSET into the Oracle scan as Oracle 12c+ row-limiting clause:
//   FETCH FIRST n ROWS ONLY
//   OFFSET m ROWS FETCH NEXT n ROWS ONLY
static void OptimizeOracleScanLimitPushdown(unique_ptr<LogicalOperator> &op) {
	if (op->type == LogicalOperatorType::LOGICAL_LIMIT) {
		auto &limit = op->Cast<LogicalLimit>();
		reference<LogicalOperator> child = *op->children[0];

		while (child.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
			child = *child.get().children[0];
		}

		if (child.get().type != LogicalOperatorType::LOGICAL_GET) {
			OptimizeOracleScanLimitPushdown(op->children[0]);
			return;
		}

		auto &get = child.get().Cast<LogicalGet>();
		if (!OracleCatalog::IsOracleScan(get.function.GetName().GetIdentifierName())) {
			OptimizeOracleScanLimitPushdown(op->children[0]);
			return;
		}

		switch (limit.limit_val.Type()) {
		case LimitNodeType::CONSTANT_VALUE:
		case LimitNodeType::UNSET:
			break;
		default:
			OptimizeOracleScanLimitPushdown(op->children[0]);
			return;
		}
		switch (limit.offset_val.Type()) {
		case LimitNodeType::CONSTANT_VALUE:
		case LimitNodeType::UNSET:
			break;
		default:
			OptimizeOracleScanLimitPushdown(op->children[0]);
			return;
		}

		auto &bind_data = get.bind_data->Cast<OracleBindData>();
		if (bind_data.max_threads != 1 || !bind_data.can_use_main_thread) {
			OptimizeOracleScanLimitPushdown(op->children[0]);
			return;
		}

		// Build Oracle 12c+ row-limiting clause
		bool has_limit = (limit.limit_val.Type() == LimitNodeType::CONSTANT_VALUE);
		bool has_offset = (limit.offset_val.Type() == LimitNodeType::CONSTANT_VALUE);

		if (!has_limit && !has_offset) {
			OptimizeOracleScanLimitPushdown(op->children[0]);
			return;
		}

		string generated_limit_clause;
		if (has_offset && has_limit) {
			generated_limit_clause = " OFFSET " +
			                         to_string(limit.offset_val.GetConstantValue()) +
			                         " ROWS FETCH NEXT " +
			                         to_string(limit.limit_val.GetConstantValue()) +
			                         " ROWS ONLY";
		} else if (has_limit) {
			generated_limit_clause = " FETCH FIRST " +
			                         to_string(limit.limit_val.GetConstantValue()) +
			                         " ROWS ONLY";
		} else {
			// offset only - emit a large FETCH NEXT
			generated_limit_clause = " OFFSET " +
			                         to_string(limit.offset_val.GetConstantValue()) +
			                         " ROWS FETCH NEXT 2147483647 ROWS ONLY";
		}

		bind_data.limit = generated_limit_clause;
		op = std::move(op->children[0]);
		return;
	}

	for (auto &child : op->children) {
		OptimizeOracleScanLimitPushdown(child);
	}
}

static void GatherOracleScans(LogicalOperator &op, OracleOperators &result) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<LogicalGet>();
		if (!OracleCatalog::IsOracleScan(get.function.GetName().GetIdentifierName())) {
			return;
		}
		auto &bind_data = get.bind_data->Cast<OracleBindData>();
		auto catalog = bind_data.GetCatalog();
		if (!catalog) {
			// standalone oracle_scan() call - always stream
			return;
		}
		result.scans[*catalog].push_back(get);
	}
	for (auto &child : op.children) {
		GatherOracleScans(*child, result);
	}
}

void OracleOptimizer::Optimize(OptimizerExtensionInput &input,
                                unique_ptr<LogicalOperator> &plan) {
	// Attempt to push LIMIT/OFFSET into the scan
	OptimizeOracleScanLimitPushdown(plan);

	// Determine whether scans can stream or must materialise
	OracleOperators operators;
	GatherOracleScans(*plan, operators);
	if (operators.scans.empty()) {
		return;
	}
	for (auto &entry : operators.scans) {
		bool multiple_scans = entry.second.size() > 1;
		for (auto &scan : entry.second) {
			auto &bind_data = scan.get().bind_data->Cast<OracleBindData>();
			if (multiple_scans) {
				// Oracle always runs single-threaded; materialise when multiple scans
				bind_data.requires_materialization = true;
				bind_data.can_use_main_thread = true;
			} else {
				bind_data.requires_materialization = false;
				bind_data.can_use_main_thread = true;
			}
		}
	}
}

} // namespace duckdb
