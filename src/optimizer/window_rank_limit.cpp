#include "duckdb/optimizer/window_rank_limit.hpp"

#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

namespace {

//! Follows a column binding down through projections to the window that produces it
optional_ptr<LogicalWindow> FindWindow(LogicalOperator &start, ColumnBinding binding, idx_t &window_column) {
	reference<LogicalOperator> current = start;
	while (current.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = current.get().Cast<LogicalProjection>();
		if (projection.table_index != binding.table_index) {
			return nullptr;
		}
		auto &expr = *projection.expressions[binding.column_index];
		if (expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			return nullptr;
		}
		binding = expr.Cast<BoundColumnRefExpression>().Binding();
		current = *current.get().children[0];
	}
	if (current.get().type != LogicalOperatorType::LOGICAL_WINDOW) {
		return nullptr;
	}
	auto &window = current.get().Cast<LogicalWindow>();
	if (window.window_index != binding.table_index) {
		return nullptr;
	}
	window_column = binding.column_index;
	return &window;
}

} // namespace

WindowRankLimit::WindowRankLimit(ClientContext &context_p,
                                         optional_ptr<column_binding_map_t<unique_ptr<BaseStatistics>>> stats_p)
    : context(context_p), stats(stats_p) {
}

bool WindowRankLimit::PartitionsTooSmall(LogicalOperator &input, const vector<unique_ptr<Expression>> &partitions,
                                             idx_t limit) {
	if (partitions.empty() || !stats) {
		return false;
	}
	const auto rows = input.EstimateCardinality(context);
	idx_t partition_count = 1;
	for (auto &partition : partitions) {
		if (partition->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			return false;
		}
		auto entry = stats->find(partition->Cast<BoundColumnRefExpression>().Binding());
		if (entry == stats->end() || !entry->second) {
			return false;
		}
		const auto distinct = entry->second->GetDistinctCount();
		if (distinct == 0) {
			return false;
		}
		partition_count = MinValue<idx_t>(rows, partition_count * distinct);
	}
	// a row is only dropped once N better rows of its partition were seen, which needs a few times N rows each
	return rows < 4 * limit * partition_count;
}

unique_ptr<LogicalOperator> WindowRankLimit::Optimize(unique_ptr<LogicalOperator> op) {
	Visit(op);
	return op;
}

void WindowRankLimit::Visit(unique_ptr<LogicalOperator> &op) {
	if (op->type == LogicalOperatorType::LOGICAL_FILTER) {
		TryMark(*op);
	}
	for (auto &child : op->children) {
		Visit(child);
	}
}

bool WindowRankLimit::TryMark(LogicalOperator &filter_op) {
	auto &filter = filter_op.Cast<LogicalFilter>();
	for (auto &filter_expr : filter.expressions) {
		const auto comparison = filter_expr->GetExpressionType();
		if (comparison != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
		    comparison != ExpressionType::COMPARE_LESSTHAN) {
			continue;
		}
		auto &compare = filter_expr->Cast<BoundFunctionExpression>();
		auto &left = BoundComparisonExpression::Left(compare);
		auto &right = BoundComparisonExpression::Right(compare);
		if (left.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF ||
		    right.GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
			continue;
		}
		auto &limit_value = right.Cast<BoundConstantExpression>().GetValue();
		if (limit_value.IsNull() || limit_value.type() != LogicalType::BIGINT) {
			continue;
		}
		auto limit = limit_value.GetValue<int64_t>();
		if (comparison == ExpressionType::COMPARE_LESSTHAN) {
			limit--;
		}
		if (limit < 1 || limit > MAX_LIMIT) {
			continue;
		}

		idx_t window_column;
		auto window = FindWindow(*filter.children[0], left.Cast<BoundColumnRefExpression>().Binding(), window_column);
		if (!window) {
			continue;
		}
		auto &rank_expr = *window->expressions[window_column];
		if (rank_expr.GetExpressionType() != ExpressionType::WINDOW_RANK) {
			continue;
		}
		// every window computed here must see the same rows: only rank() over the same partition and order
		bool same_windows = true;
		for (auto &expr : window->expressions) {
			if (!expr->Equals(rank_expr)) {
				same_windows = false;
			}
		}
		auto &window_expr = rank_expr.Cast<BoundWindowExpression>();
		if (!same_windows || window_expr.OrderBy().empty() || !window_expr.ArgOrders().empty()) {
			continue;
		}
		if (window->rank_limit != 0) {
			return false; // already marked
		}
		if (PartitionsTooSmall(*window->children[0], window_expr.Partitions(), NumericCast<idx_t>(limit))) {
			continue;
		}
		window->rank_limit = NumericCast<idx_t>(limit);
		return true;
	}
	return false;
}

} // namespace duckdb
