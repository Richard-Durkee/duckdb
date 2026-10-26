#include "duckdb/optimizer/scalar_aggregate_merge.hpp"

#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

//! One scalar subquery of a cross product: projections over an ungrouped aggregate over projections and filters
//! over a scan, with the aggregates, the filter and the outputs rewritten over the scan's own columns
struct ScalarAggregateBranch {
	//! The slot of the branch in the cross product
	unique_ptr<LogicalOperator> *slot;
	LogicalGet *get;
	//! The binding the operator above the branch reads, and how many columns it has
	TableIndex output_index;
	//! The aggregates, over the scan's columns
	vector<unique_ptr<Expression>> aggregates;
	//! The branch's filter over the scan's columns (empty: every row)
	vector<unique_ptr<Expression>> conditions;
	//! The branch's outputs, over the aggregate's bindings
	vector<unique_ptr<Expression>> outputs;
	TableIndex aggregate_index;
};

ScalarAggregateMerge::ScalarAggregateMerge(Optimizer &optimizer) : optimizer(optimizer) {
}

//! Replace the column references of 'expr' with copies of their definitions in 'definitions'. False if a reference
//! has no definition (it reads something outside the branch)
static bool Inline(unique_ptr<Expression> &expr, const column_binding_map_t<unique_ptr<Expression>> &definitions) {
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr->Cast<BoundColumnRefExpression>();
		if (colref.Depth() > 0) {
			return false;
		}
		auto entry = definitions.find(colref.Binding());
		if (entry == definitions.end()) {
			return false;
		}
		expr = entry->second->Copy();
		return true;
	}
	bool ok = true;
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		if (ok) {
			ok = Inline(child, definitions);
		}
	});
	return ok;
}

static bool HasSubquery(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { found = found || HasSubquery(child); });
	return found;
}

static bool Usable(const Expression &expr) {
	return !expr.IsVolatile() && !HasSubquery(expr);
}

//! The scan's columns as definitions of its own bindings
static column_binding_map_t<unique_ptr<Expression>> ScanDefinitions(LogicalGet &get) {
	column_binding_map_t<unique_ptr<Expression>> definitions;
	// not GetColumnBindings: a scan that reads no column still reports one binding
	auto &column_ids = get.GetColumnIds();
	for (idx_t i = 0; i < column_ids.size(); i++) {
		ColumnBinding binding(get.table_index, ProjectionIndex(i));
		definitions.emplace(binding, make_uniq<BoundColumnRefExpression>(get.GetColumnType(column_ids[i]), binding));
	}
	return definitions;
}

//! Whether 'op' is a scalar subquery branch, and if so its aggregates, filter and outputs over its scan
static bool MatchBranch(unique_ptr<LogicalOperator> &slot, ScalarAggregateBranch &branch) {
	vector<reference<LogicalProjection>> upper;
	auto current = slot.get();
	while (current->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		upper.push_back(current->Cast<LogicalProjection>());
		current = current->children[0].get();
	}
	if (current->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return false;
	}
	auto &aggregate = current->Cast<LogicalAggregate>();
	if (!aggregate.groups.empty() || !aggregate.grouping_sets.empty() || !aggregate.grouping_functions.empty() ||
	    aggregate.expressions.empty()) {
		return false;
	}
	// the operators between the aggregate and the scan, top to bottom
	vector<reference<LogicalOperator>> lower;
	current = aggregate.children[0].get();
	while (current->type == LogicalOperatorType::LOGICAL_PROJECTION ||
	       current->type == LogicalOperatorType::LOGICAL_FILTER) {
		lower.push_back(*current);
		current = current->children[0].get();
	}
	if (current->type != LogicalOperatorType::LOGICAL_GET || !current->children.empty()) {
		return false;
	}
	auto &get = current->Cast<LogicalGet>();
	if (!get.GetTable() || get.table_filters.HasFilters() || get.dynamic_filters || !get.projection_ids.empty() ||
	    get.at_clause || get.ordinality_idx.IsValid()) {
		return false;
	}
	for (auto &filter : lower) {
		if (filter.get().type == LogicalOperatorType::LOGICAL_FILTER &&
		    !filter.get().Cast<LogicalFilter>().projection_map.empty()) {
			return false;
		}
	}
	auto definitions = ScanDefinitions(get);
	for (idx_t i = lower.size(); i > 0; i--) {
		auto &op = lower[i - 1].get();
		if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
			for (auto &condition : op.expressions) {
				if (!Usable(*condition)) {
					return false;
				}
				auto copy = condition->Copy();
				if (!Inline(copy, definitions)) {
					return false;
				}
				branch.conditions.push_back(std::move(copy));
			}
			continue;
		}
		auto &projection = op.Cast<LogicalProjection>();
		column_binding_map_t<unique_ptr<Expression>> next;
		for (idx_t col = 0; col < projection.expressions.size(); col++) {
			if (!Usable(*projection.expressions[col])) {
				return false;
			}
			auto copy = projection.expressions[col]->Copy();
			if (!Inline(copy, definitions)) {
				return false;
			}
			next.emplace(ColumnBinding(projection.table_index, ProjectionIndex(col)), std::move(copy));
		}
		definitions = std::move(next);
	}
	for (auto &expr : aggregate.expressions) {
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE || !Usable(*expr)) {
			return false;
		}
		auto copy = expr->Copy();
		if (!Inline(copy, definitions)) {
			return false;
		}
		branch.aggregates.push_back(std::move(copy));
	}
	// the outputs above the aggregate, over its bindings
	column_binding_map_t<unique_ptr<Expression>> outputs;
	for (idx_t i = 0; i < aggregate.expressions.size(); i++) {
		ColumnBinding binding(aggregate.aggregate_index, ProjectionIndex(i));
		outputs.emplace(binding,
		                make_uniq<BoundColumnRefExpression>(aggregate.expressions[i]->GetReturnType(), binding));
	}
	auto output_index = aggregate.aggregate_index;
	idx_t output_count = aggregate.expressions.size();
	for (idx_t i = upper.size(); i > 0; i--) {
		auto &projection = upper[i - 1].get();
		column_binding_map_t<unique_ptr<Expression>> next;
		for (idx_t col = 0; col < projection.expressions.size(); col++) {
			if (!Usable(*projection.expressions[col])) {
				return false;
			}
			auto copy = projection.expressions[col]->Copy();
			if (!Inline(copy, outputs)) {
				return false;
			}
			next.emplace(ColumnBinding(projection.table_index, ProjectionIndex(col)), std::move(copy));
		}
		outputs = std::move(next);
		output_index = projection.table_index;
		output_count = projection.expressions.size();
	}
	for (idx_t col = 0; col < output_count; col++) {
		branch.outputs.push_back(std::move(outputs[ColumnBinding(output_index, ProjectionIndex(col))]));
	}
	branch.slot = &slot;
	branch.get = &get;
	branch.output_index = output_index;
	branch.aggregate_index = aggregate.aggregate_index;
	return true;
}

//! Whether two branches scan the same table the same way
static bool SameScan(const LogicalGet &a, const LogicalGet &b) {
	if (a.function.GetName() != b.function.GetName() || a.GetTable().get() != b.GetTable().get() ||
	    a.parameters != b.parameters || a.named_parameters.size() != b.named_parameters.size()) {
		return false;
	}
	for (auto &entry : a.named_parameters) {
		auto other = b.named_parameters.find(entry.first);
		if (other == b.named_parameters.end() || other->second != entry.second) {
			return false;
		}
	}
	return true;
}

static unique_ptr<Expression> Conjunction(ExpressionType type, vector<unique_ptr<Expression>> parts) {
	if (parts.empty()) {
		return nullptr;
	}
	auto result = std::move(parts[0]);
	for (idx_t i = 1; i < parts.size(); i++) {
		result = make_uniq<BoundConjunctionExpression>(type, std::move(result), std::move(parts[i]));
	}
	return result;
}

//! Rebind the scan columns 'expr' reads from 'from' to columns of 'to', adding the ones 'to' does not read yet
static void RebindToScan(Expression &expr, LogicalGet &from, LogicalGet &to) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &binding = expr.Cast<BoundColumnRefExpression>().BindingMutable();
		D_ASSERT(binding.table_index == from.table_index);
		auto &column = from.GetColumnIds()[binding.column_index.GetIndex()];
		auto &target = to.GetMutableColumnIds();
		idx_t position = target.size();
		for (idx_t i = 0; i < target.size(); i++) {
			if (target[i] == column) {
				position = i;
				break;
			}
		}
		if (position == target.size()) {
			target.push_back(column);
		}
		binding = ColumnBinding(to.table_index, ProjectionIndex(position));
		return;
	}
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) { RebindToScan(child, from, to); });
}

//! A bound a condition puts on a column: a comparison of the column with a constant
struct ColumnBound {
	ColumnBinding column;
	Value constant;
	bool lower;
	bool inclusive;
};

static void AddBound(const Expression &column, const Expression &constant, ExpressionType type,
                     vector<ColumnBound> &bounds) {
	if (column.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
	    constant.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
	    column.GetReturnType() != constant.GetReturnType()) {
		return;
	}
	auto &value = constant.Cast<BoundConstantExpression>().GetValue();
	if (value.IsNull()) {
		return;
	}
	auto binding = column.Cast<BoundColumnRefExpression>().Binding();
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		bounds.push_back({binding, value, true, true});
		bounds.push_back({binding, value, false, true});
		break;
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		bounds.push_back({binding, value, true, type == ExpressionType::COMPARE_GREATERTHANOREQUALTO});
		break;
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		bounds.push_back({binding, value, false, type == ExpressionType::COMPARE_LESSTHANOREQUALTO});
		break;
	default:
		break;
	}
}

//! The bounds the conjuncts of 'condition' put on columns
static vector<ColumnBound> ConditionBounds(const Expression &condition) {
	vector<ColumnBound> bounds;
	vector<reference<const Expression>> conjuncts;
	conjuncts.push_back(condition);
	for (idx_t i = 0; i < conjuncts.size(); i++) {
		auto &expr = conjuncts[i].get();
		if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
			for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
				conjuncts.push_back(*child);
			}
		} else if (expr.GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
			auto &between = expr.Cast<BoundFunctionExpression>();
			auto &input = BoundBetweenExpression::Input(between);
			AddBound(input, BoundBetweenExpression::LowerBound(between),
			         BoundBetweenExpression::LowerComparisonType(between), bounds);
			AddBound(input, BoundBetweenExpression::UpperBound(between),
			         BoundBetweenExpression::UpperComparisonType(between), bounds);
		} else if (BoundComparisonExpression::IsComparison(expr)) {
			auto &comparison = expr.Cast<BoundFunctionExpression>();
			auto &left = BoundComparisonExpression::Left(comparison);
			auto &right = BoundComparisonExpression::Right(comparison);
			if (left.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
				AddBound(right, left, FlipComparisonExpression(expr.GetExpressionType()), bounds);
			} else {
				AddBound(left, right, expr.GetExpressionType(), bounds);
			}
		}
	}
	return bounds;
}

//! The bound of one side of 'column' that holds for every condition, if each condition bounds that side
static optional_ptr<const ColumnBound> CommonBound(const vector<vector<ColumnBound>> &bounds, ColumnBinding column,
                                                   bool lower) {
	optional_ptr<const ColumnBound> result;
	for (auto &condition : bounds) {
		optional_ptr<const ColumnBound> found;
		for (auto &bound : condition) {
			if (bound.column == column && bound.lower == lower) {
				found = bound;
				break;
			}
		}
		if (!found) {
			return nullptr;
		}
		// the loosest bound: the lowest lower bound, the highest upper bound, inclusive on a tie
		if (!result || (lower ? found->constant < result->constant : result->constant < found->constant) ||
		    (found->constant == result->constant && found->inclusive)) {
			result = found;
		}
	}
	return result;
}

//! A filter that keeps every row any of the conditions keeps. When each condition is a range of one column, the range
//! that covers them all: cheaper than their OR, which the scan evaluates term by term for every row (TPC-DS q9's five
//! buckets of ss_quantity cover every non-NULL value)
static unique_ptr<Expression> AnyCondition(vector<unique_ptr<Expression>> conditions) {
	vector<vector<ColumnBound>> bounds;
	for (auto &condition : conditions) {
		bounds.push_back(ConditionBounds(*condition));
	}
	for (auto &candidate : bounds[0]) {
		bool in_every = true;
		for (auto &condition : bounds) {
			bool found = false;
			for (auto &bound : condition) {
				found = found || bound.column == candidate.column;
			}
			in_every = in_every && found;
		}
		if (!in_every) {
			continue;
		}
		auto type = LogicalType(candidate.constant.type());
		vector<unique_ptr<Expression>> parts;
		for (bool lower : {true, false}) {
			auto bound = CommonBound(bounds, candidate.column, lower);
			if (!bound) {
				continue;
			}
			auto comparison = lower ? (bound->inclusive ? ExpressionType::COMPARE_GREATERTHANOREQUALTO
			                                            : ExpressionType::COMPARE_GREATERTHAN)
			                        : (bound->inclusive ? ExpressionType::COMPARE_LESSTHANOREQUALTO
			                                            : ExpressionType::COMPARE_LESSTHAN);
			parts.push_back(BoundComparisonExpression::Create(
			    comparison, make_uniq<BoundColumnRefExpression>(type, candidate.column),
			    make_uniq<BoundConstantExpression>(bound->constant)));
		}
		if (parts.empty()) {
			// a comparison is never true for NULL
			auto not_null =
			    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
			not_null->GetChildrenMutable().push_back(make_uniq<BoundColumnRefExpression>(type, candidate.column));
			parts.push_back(std::move(not_null));
		}
		return Conjunction(ExpressionType::CONJUNCTION_AND, std::move(parts));
	}
	return Conjunction(ExpressionType::CONJUNCTION_OR, std::move(conditions));
}

unique_ptr<LogicalOperator> ScalarAggregateMerge::MergeBranches(vector<ScalarAggregateBranch> &branches) {
	auto &base = branches[0];
	auto &scan = *base.get;
	// detach the base scan from its branch: it becomes the scan of the merged branch
	unique_ptr<LogicalOperator> *scan_slot = base.slot;
	while ((*scan_slot)->type != LogicalOperatorType::LOGICAL_GET) {
		scan_slot = &(*scan_slot)->children[0];
	}
	auto merged_scan = std::move(*scan_slot);

	vector<unique_ptr<Expression>> aggregates;
	vector<unique_ptr<Expression>> any_condition;
	bool every_row = false;
	auto merged_index = optimizer.binder.GenerateTableIndex();
	auto aggregate_index = optimizer.binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> outputs;
	for (auto &branch : branches) {
		for (auto &condition : branch.conditions) {
			RebindToScan(*condition, *branch.get, scan);
		}
		auto condition = Conjunction(ExpressionType::CONJUNCTION_AND, std::move(branch.conditions));
		if (condition) {
			// subqueries over the same rows (count, sum and avg of one bucket) add their condition once
			bool seen = false;
			for (auto &other : any_condition) {
				seen = seen || other->Equals(*condition);
			}
			if (!seen) {
				any_condition.push_back(condition->Copy());
			}
		} else {
			every_row = true;
		}
		idx_t offset = aggregates.size();
		for (auto &expr : branch.aggregates) {
			RebindToScan(*expr, *branch.get, scan);
			auto &aggregate = expr->Cast<BoundAggregateExpression>();
			if (condition) {
				auto &filter = aggregate.GetFilterMutable();
				filter = filter ? make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
				                                                        condition->Copy(), std::move(filter))
				                : condition->Copy();
			}
			aggregates.push_back(std::move(expr));
		}
		// the branch's outputs read its own aggregate: point them at its aggregates in the merged one
		column_binding_map_t<unique_ptr<Expression>> merged_aggregates;
		for (idx_t i = offset; i < aggregates.size(); i++) {
			merged_aggregates.emplace(
			    ColumnBinding(branch.aggregate_index, ProjectionIndex(i - offset)),
			    make_uniq<BoundColumnRefExpression>(aggregates[i]->GetReturnType(),
			                                        ColumnBinding(aggregate_index, ProjectionIndex(i))));
		}
		for (idx_t col = 0; col < branch.outputs.size(); col++) {
			auto output = std::move(branch.outputs[col]);
			auto inlined = Inline(output, merged_aggregates);
			D_ASSERT(inlined);
			(void)inlined;
			replacer.replacement_bindings.emplace_back(ColumnBinding(branch.output_index, ProjectionIndex(col)),
			                                           ColumnBinding(merged_index, ProjectionIndex(outputs.size())));
			outputs.push_back(std::move(output));
		}
	}
	unique_ptr<LogicalOperator> input = std::move(merged_scan);
	if (!every_row) {
		auto filter = make_uniq<LogicalFilter>(AnyCondition(std::move(any_condition)));
		filter->children.push_back(std::move(input));
		input = std::move(filter);
	}
	auto group_index = optimizer.binder.GenerateTableIndex();
	auto aggregate = make_uniq<LogicalAggregate>(group_index, aggregate_index, std::move(aggregates));
	aggregate->children.push_back(std::move(input));
	auto projection = make_uniq<LogicalProjection>(merged_index, std::move(outputs));
	projection->children.push_back(std::move(aggregate));
	return std::move(projection);
}

static void CollectLeaves(unique_ptr<LogicalOperator> &op, vector<unique_ptr<LogicalOperator> *> &leaves) {
	if (op->type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
		CollectLeaves(op->children[0], leaves);
		CollectLeaves(op->children[1], leaves);
		return;
	}
	leaves.push_back(&op);
}

void ScalarAggregateMerge::MergeCrossProduct(unique_ptr<LogicalOperator> &op) {
	vector<unique_ptr<LogicalOperator> *> leaves;
	CollectLeaves(op, leaves);
	vector<ScalarAggregateBranch> branches;
	vector<bool> is_branch(leaves.size(), false);
	for (idx_t i = 0; i < leaves.size(); i++) {
		ScalarAggregateBranch branch;
		if (MatchBranch(*leaves[i], branch)) {
			branches.push_back(std::move(branch));
			is_branch[i] = true;
		}
	}
	// group the branches by the scan they read
	vector<vector<idx_t>> groups;
	for (idx_t i = 0; i < branches.size(); i++) {
		bool placed = false;
		for (auto &group : groups) {
			if (SameScan(*branches[group[0]].get, *branches[i].get)) {
				group.push_back(i);
				placed = true;
				break;
			}
		}
		if (!placed) {
			groups.push_back({i});
		}
	}
	bool merged_any = false;
	for (auto &group : groups) {
		merged_any = merged_any || group.size() > 1;
	}
	if (!merged_any) {
		return;
	}
	vector<unique_ptr<LogicalOperator>> children;
	vector<bool> taken(leaves.size(), false);
	for (auto &group : groups) {
		if (group.size() < 2) {
			continue;
		}
		vector<ScalarAggregateBranch> members;
		for (auto index : group) {
			members.push_back(std::move(branches[index]));
		}
		for (auto &member : members) {
			for (idx_t i = 0; i < leaves.size(); i++) {
				if (leaves[i] == member.slot) {
					taken[i] = true;
				}
			}
		}
		children.push_back(MergeBranches(members));
	}
	for (idx_t i = 0; i < leaves.size(); i++) {
		if (!taken[i]) {
			children.push_back(std::move(*leaves[i]));
		}
	}
	auto result = std::move(children[0]);
	for (idx_t i = 1; i < children.size(); i++) {
		result = LogicalCrossProduct::Create(std::move(result), std::move(children[i]));
	}
	op = std::move(result);
}

void ScalarAggregateMerge::VisitOperator(unique_ptr<LogicalOperator> &op) {
	if (op->type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
		MergeCrossProduct(op);
		if (op->type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
			vector<unique_ptr<LogicalOperator> *> leaves;
			CollectLeaves(op, leaves);
			for (auto leaf : leaves) {
				VisitOperator(*leaf);
			}
			return;
		}
	}
	for (auto &child : op->children) {
		VisitOperator(child);
	}
}

unique_ptr<LogicalOperator> ScalarAggregateMerge::Optimize(unique_ptr<LogicalOperator> op) {
	VisitOperator(op);
	if (!replacer.replacement_bindings.empty()) {
		replacer.VisitOperator(*op);
		op->ResolveOperatorTypes();
	}
	return op;
}

} // namespace duckdb
