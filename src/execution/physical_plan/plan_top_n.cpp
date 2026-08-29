#include "duckdb/execution/operator/order/physical_top_n.hpp"
#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"

namespace duckdb {

//! Try to find a PhysicalHashAggregate as the direct child (or through projections)
static PhysicalHashAggregate *FindChildAggregate(PhysicalOperator &op) {
	if (op.type == PhysicalOperatorType::HASH_GROUP_BY) {
		return &op.Cast<PhysicalHashAggregate>();
	}
	if (op.type == PhysicalOperatorType::PROJECTION && !op.children.empty()) {
		return FindChildAggregate(op.children[0].get());
	}
	return nullptr;
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalTopN &op) {
	D_ASSERT(op.children.size() == 1);
	auto &plan = CreatePlan(*op.children[0]);
	auto &top_n =
	    Make<PhysicalTopN>(op.types, std::move(op.orders), NumericCast<idx_t>(op.limit), NumericCast<idx_t>(op.offset),
	                       std::move(op.dynamic_filter), op.estimated_cardinality);
	top_n.children.push_back(plan);

	// Wire TopKAggregateFilter if TopN orders by a grouping key of a direct child aggregate
	auto &top_n_op = top_n.Cast<PhysicalTopN>();
	if (top_n_op.limit <= 10000 && top_n_op.offset == 0 && top_n_op.orders.size() == 1 && top_n_op.dynamic_filter) {
		auto &order = top_n_op.orders[0];
		if (order.expression->GetExpressionType() == ExpressionType::BOUND_REF) {
			auto &ref = order.expression->Cast<BoundReferenceExpression>();
			auto *agg = FindChildAggregate(plan);
			if (agg && !agg->groupings.empty()) {
				auto &grouping = agg->groupings[0];
				auto num_groups = agg->grouped_aggregate_data.groups.size();
				auto col_idx = ref.Index();
				// Only wire if ORDER BY references a grouping column (not an aggregate result)
				if (col_idx < num_groups) {
					RadixPartitionedHashTable::TopKConfig config;
					config.limit = top_n_op.limit;
					config.group_col_index = col_idx;
					config.order_type = order.type;
					config.null_order = order.null_order;
					config.filter_data = top_n_op.dynamic_filter;
					grouping.table_data.topk_config = config;
				}
			}
		}
	}

	return top_n;
}

} // namespace duckdb
