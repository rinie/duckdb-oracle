#include "oracle_filter_pushdown.hpp"
#include "oracle_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"

// The expression-based filter transformation follows the shared database-connector library
// (github.com/duckdb/database-connector) used by the postgres/mysql/sqlite extensions.

namespace duckdb {

static string TransformExpression(const string &column_name, const Expression &expr);

static bool IsDirectReference(const Expression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_REF:
	case ExpressionClass::BOUND_COLUMN_REF:
		return true;
	default:
		return false;
	}
}

static bool IsInternalExpression(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	       TableFilterFunctions::IsTableFilterFunction(expr.Cast<BoundFunctionExpression>().Function());
}

static string GetComparisonOperator(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return "=";
	case ExpressionType::COMPARE_NOTEQUAL:
		return "<>";
	case ExpressionType::COMPARE_LESSTHAN:
		return "<";
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return "<=";
	case ExpressionType::COMPARE_GREATERTHAN:
		return ">";
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ">=";
	default:
		return string();
	}
}

//! Renders a constant as an Oracle literal, or returns an empty string if it cannot be pushed down
static string WriteConstant(const Value &val) {
	if (val.IsNull()) {
		return string();
	}
	switch (val.type().id()) {
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::CHAR:
		return OracleUtils::WriteLiteral(StringValue::Get(val));
	case LogicalTypeId::DATE: {
		int32_t y, mo, day;
		Date::Convert(val.GetValueUnsafe<date_t>(), y, mo, day);
		return StringUtil::Format("DATE '%04d-%02d-%02d'", y, mo, day);
	}
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ: {
		auto ts = val.GetValueUnsafe<timestamp_t>();
		int32_t y, mo, day, h, m, s, micros;
		Date::Convert(Timestamp::GetDate(ts), y, mo, day);
		Time::Convert(Timestamp::GetTime(ts), h, m, s, micros);
		auto literal =
		    StringUtil::Format("TIMESTAMP '%04d-%02d-%02d %02d:%02d:%02d.%06d'", y, mo, day, h, m, s, micros);
		if (val.type().id() == LogicalTypeId::TIMESTAMP_TZ) {
			// TIMESTAMP_TZ values are UTC instants
			literal += " AT TIME ZONE 'UTC'";
		}
		return literal;
	}
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE: {
		auto d = val.GetValue<double>();
		if (!std::isfinite(d)) {
			return string();
		}
		return val.ToString();
	}
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::DECIMAL:
		return val.ToString();
	default:
		return string();
	}
}

static string TransformComparison(const string &column_name, const Expression &expr) {
	auto &comparison = expr.Cast<BoundFunctionExpression>();
	auto comparison_type = comparison.GetExpressionType();
	auto &left = BoundComparisonExpression::Left(comparison);
	auto &right = BoundComparisonExpression::Right(comparison);
	const Value *constant = nullptr;
	if (IsDirectReference(left) && right.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		constant = &right.Cast<BoundConstantExpression>().GetValue();
	} else if (left.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT && IsDirectReference(right)) {
		constant = &left.Cast<BoundConstantExpression>().GetValue();
		comparison_type = FlipComparisonExpression(comparison_type);
	} else {
		return string();
	}
	auto op = GetComparisonOperator(comparison_type);
	auto constant_string = WriteConstant(*constant);
	if (op.empty() || constant_string.empty()) {
		return string();
	}
	return column_name + " " + op + " " + constant_string;
}

static string TransformConjunction(const string &column_name, const vector<unique_ptr<Expression>> &children,
                                   const string &op) {
	vector<string> parts;
	for (auto &child : children) {
		auto part = TransformExpression(column_name, *child);
		if (part.empty()) {
			if (op == "AND" && IsInternalExpression(*child)) {
				// internal filters only prune rows, so an AND can go without them
				continue;
			}
			// without one of its children the conjunction would select other rows
			return string();
		}
		parts.push_back(std::move(part));
	}
	if (parts.empty()) {
		return string();
	}
	return "(" + StringUtil::Join(parts, " " + op + " ") + ")";
}

static string TransformIn(const string &column_name, const BoundOperatorExpression &op) {
	auto &children = op.GetChildren();
	if (children.size() < 2 || !IsDirectReference(*children[0])) {
		return string();
	}
	string in_list;
	for (idx_t i = 1; i < children.size(); i++) {
		if (children[i]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			return string();
		}
		auto constant_string = WriteConstant(children[i]->Cast<BoundConstantExpression>().GetValue());
		if (constant_string.empty()) {
			return string();
		}
		if (!in_list.empty()) {
			in_list += ", ";
		}
		in_list += constant_string;
	}
	return column_name + " IN (" + in_list + ")";
}

static string TransformExpression(const string &column_name, const Expression &expr) {
	if (BoundComparisonExpression::IsComparison(expr)) {
		return TransformComparison(column_name, expr);
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conjunction = expr.Cast<BoundConjunctionExpression>();
		switch (conjunction.GetExpressionType()) {
		case ExpressionType::CONJUNCTION_AND:
			return TransformConjunction(column_name, conjunction.GetChildren(), "AND");
		case ExpressionType::CONJUNCTION_OR:
			return TransformConjunction(column_name, conjunction.GetChildren(), "OR");
		default:
			return string();
		}
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = expr.Cast<BoundOperatorExpression>();
		auto &children = op.GetChildren();
		if (children.empty() || !IsDirectReference(*children[0])) {
			return string();
		}
		switch (op.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL:
			return column_name + " IS NULL";
		case ExpressionType::OPERATOR_IS_NOT_NULL:
			return column_name + " IS NOT NULL";
		case ExpressionType::COMPARE_IN:
			return TransformIn(column_name, op);
		default:
			return string();
		}
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.Function().GetName() == OptionalFilterScalarFun::NAME && func.BindInfo()) {
			auto &data = func.BindInfo()->Cast<OptionalFilterFunctionData>();
			return data.child_filter_expr ? TransformExpression(column_name, *data.child_filter_expr) : string();
		}
		if (func.Function().GetName() == SelectivityOptionalFilterScalarFun::NAME && func.BindInfo()) {
			auto &data = func.BindInfo()->Cast<SelectivityOptionalFilterFunctionData>();
			return data.child_filter_expr ? TransformExpression(column_name, *data.child_filter_expr) : string();
		}
		switch (expr.GetExpressionType()) {
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_NOTEQUAL:
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			return TransformComparison(column_name, expr);
		default:
			return string();
		}
	}
	default:
		return string();
	}
}

bool OracleFilterPushdown::CanPushExpressionDown(ClientContext &, const LogicalGet &, Expression &expr) {
	return !TransformExpression("dummy", expr).empty();
}

string OracleFilterPushdown::TransformFilters(const vector<column_t> &column_ids,
                                              optional_ptr<TableFilterSet> filters, const vector<string> &names) {
	if (!filters || !filters->HasFilters()) {
		return string();
	}
	vector<string> filter_parts;
	for (auto &entry : *filters) {
		auto column_id = column_ids[entry.GetIndex()];
		if (IsVirtualColumn(column_id)) {
			continue;
		}
		auto column_name = OracleUtils::QuoteIdentifier(names[column_id]);
		auto &filter = entry.Filter();
		auto &expr = *ExpressionFilter::GetExpressionFilter(filter, "OracleFilterPushdown::TransformFilters").expr;
		auto filter_text = TransformExpression(column_name, expr);
		if (filter_text.empty()) {
			if (IsInternalExpression(expr)) {
				continue;
			}
			throw NotImplementedException("Unsupported filter pushdown, set ora_experimental_filter_pushdown=false "
			                              "to disable pushdowns. Problematic filter: \"%s\"",
			                              expr.ToString());
		}
		filter_parts.push_back(std::move(filter_text));
	}
	return StringUtil::Join(filter_parts, " AND ");
}

} // namespace duckdb
