#include "duckdb/execution/operator/order/physical_top_n.hpp"
#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"

namespace duckdb {

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
	auto &top_n_base =
	    Make<PhysicalTopN>(op.types, std::move(op.orders), NumericCast<idx_t>(op.limit), NumericCast<idx_t>(op.offset),
	                       std::move(op.dynamic_filter), op.estimated_cardinality);
	top_n_base.children.push_back(plan);

	// Cast to access PhysicalTopN-specific members
	auto &top_n = top_n_base.Cast<PhysicalTopN>();

	// === Top-K aggregate optimization wiring ===
	if (top_n.dynamic_filter && top_n.orders.size() == 1 && top_n.limit <= 10000) {
		auto *agg = FindChildAggregate(plan);
		if (agg && agg->groupings.size() == 1) {
			auto &order_expr = top_n.orders[0].expression;
			if (order_expr->GetExpressionType() == ExpressionType::BOUND_REF) {
				auto &ref = order_expr->Cast<BoundReferenceExpression>();
				auto num_groups = agg->groupings[0].table_data.group_types.size();
				idx_t col_idx = ref.Index();
				if (col_idx < num_groups) {
					bool has_distinct = false;
					for (auto &aggr_expr : agg->grouped_aggregate_data.aggregates) {
						if (aggr_expr->Cast<BoundAggregateExpression>().IsDistinct()) {
							has_distinct = true;
							break;
						}
					}
					if (!has_distinct) {
						agg->topk_config.group_key_index = col_idx;
						agg->topk_config.limit = top_n.limit + top_n.offset;
						agg->topk_config.order = top_n.orders[0].type;
						agg->topk_config.filter_data = top_n.dynamic_filter;
						agg->topk_config.value_type = order_expr->GetReturnType();
					}
				}
			}
		}
	}

	return top_n_base;
}

} // namespace duckdb
