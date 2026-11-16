#include "duckdb/optimizer/group_key_encoding_cost.hpp"

#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

namespace {

//! CPU nanoseconds that a string group key costs per row that reaches the aggregate (hash, compare, copy, gather)
//! beyond the integer id that replaces it
double BenefitPerRow() {
	return 30;
}
//! CPU nanoseconds that a string saves per group of the hash table (the string is copied into the group, and the larger
//! rows miss the cache more)
double BenefitPerGroup() {
	return 150;
}
//! Every grouping set level after the first aggregates the output of the level below it
double CascadeWeight() {
	return 0.4;
}
//! CPU nanoseconds to intern one string of the relation (hash, shard lock, probe, insert on first sight)
double InternPerRow() {
	return 250;
}
//! CPU nanoseconds to look up one output group
double ResolvePerGroup() {
	return 25;
}
//! The benefit has to be this many times the cost
double SafetyFactor() {
	return 2.0;
}
//! The saving has to be at least this many nanoseconds on the critical path
double MinimumSavingNs() {
	return 10000000;
}
//! A dictionary of fewer rows than this has few distinct strings: the strings and the hash table of the aggregate stay
//! in the cache, and hashing and comparing them costs little more than integers, so the saving is scaled to zero ...
constexpr idx_t CACHE_RESIDENT_ROWS = 10000;
//! ... and reaches its full size with this many rows
constexpr idx_t FULL_SAVING_ROWS = 100000;
//! Rows of a scan that one thread processes on its own (a row group)
constexpr idx_t SCAN_UNIT_ROWS = 122880;

//! The relation is a free rider if its dictionary costs less than this share of the benefit of the keys that are
//! encoded
double FreeRiderShare() {
	return 0.02;
}

//! An aggregate below filters and projections: the shape that the pushdown of a partial aggregate below a join leaves
bool IsAggregateChain(const LogicalOperator &op) {
	auto current = &op;
	while (current->type == LogicalOperatorType::LOGICAL_PROJECTION ||
	       current->type == LogicalOperatorType::LOGICAL_FILTER) {
		current = current->children[0].get();
	}
	return current->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY;
}

//! The rows that a key join of an aggregate returns, compared to the rows of the other side. The aggregate was grouped
//! by the join key (and a few more columns), so each of its rows matches about one row of the other side
constexpr idx_t PRE_AGGREGATE_JOIN_FACTOR = 2;

} // namespace

idx_t PreAggregatedJoinRows(idx_t other_side_rows) {
	return PRE_AGGREGATE_JOIN_FACTOR * other_side_rows;
}

GroupKeyEncodingDecision DecideGroupKeyEncoding(const GroupKeyEncodingCostInput &input) {
	const double b = BenefitPerRow();
	const double c = InternPerRow();
	const double r = ResolvePerGroup();
	const double s = BenefitPerGroup();
	const double cascade = CascadeWeight();
	const double k = SafetyFactor();
	const double floor_ns = MinimumSavingNs();
	GroupKeyEncodingDecision result;
	const double threads = static_cast<double>(MaxValue<idx_t>(input.threads, 1));
	const double levels = static_cast<double>(MaxValue<idx_t>(input.grouping_sets, 1));
	// the relation is scanned by as many threads as it has row groups
	const double scan_threads =
	    MinValue<double>(threads, MaxValue<double>(1, static_cast<double>(input.dimension_rows) / SCAN_UNIT_ROWS));
	const double groups = static_cast<double>(MinValue<idx_t>(input.group_rows, input.input_rows));
	const double cache_factor =
	    MinValue<double>(1, MaxValue<double>(0, static_cast<double>(static_cast<int64_t>(input.dimension_rows) -
	                                                                static_cast<int64_t>(CACHE_RESIDENT_ROWS)) /
	                                                (FULL_SAVING_ROWS - CACHE_RESIDENT_ROWS)));
	result.benefit_cpu_ns =
	    (static_cast<double>(input.input_rows) * (1 + cascade * (levels - 1)) * b + groups * s) * cache_factor;
	result.cost_cpu_ns = static_cast<double>(input.dimension_rows) * c + groups * r;
	result.benefit_wall_ns = result.benefit_cpu_ns / threads;
	result.cost_wall_ns = static_cast<double>(input.dimension_rows) * c / scan_threads + groups * r / threads;
	result.encode = result.benefit_cpu_ns >= k * result.cost_cpu_ns &&
	                result.benefit_wall_ns >= k * result.cost_wall_ns &&
	                result.benefit_wall_ns - result.cost_wall_ns >= floor_ns;
	return result;
}

bool IsFreeRider(const GroupKeyEncodingDecision &decision, double encoded_benefit_cpu_ns) {
	return decision.cost_cpu_ns <= FreeRiderShare() * encoded_benefit_cpu_ns;
}

idx_t BoundedInputRows(ClientContext &context, LogicalOperator &op) {
	idx_t estimate = op.EstimateCardinality(context);
	idx_t result = estimate;
	idx_t largest_child = 0;
	vector<idx_t> child_rows;
	for (auto &child : op.children) {
		child_rows.push_back(BoundedInputRows(context, *child));
		largest_child = MaxValue<idx_t>(largest_child, child_rows.back());
	}
	if (!op.children.empty()) {
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
		case LogicalOperatorType::LOGICAL_UNION:
		case LogicalOperatorType::LOGICAL_EXCEPT:
		case LogicalOperatorType::LOGICAL_INTERSECT:
		case LogicalOperatorType::LOGICAL_UNNEST:
		case LogicalOperatorType::LOGICAL_RECURSIVE_CTE:
		case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
		case LogicalOperatorType::LOGICAL_CTE_REF:
			break;
		default:
			// filters, projections, aggregates, windows and key joins return at most what their larger input has
			result = MinValue<idx_t>(estimate, largest_child);
			if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN && op.children.size() == 2) {
				const auto left_aggregate = IsAggregateChain(*op.children[0]);
				const auto right_aggregate = IsAggregateChain(*op.children[1]);
				if (left_aggregate != right_aggregate) {
					result = MinValue<idx_t>(result, PreAggregatedJoinRows(child_rows[left_aggregate ? 1 : 0]));
				}
			}
		}
	}
	return result;
}

} // namespace duckdb
