#include "duckdb/optimizer/rule/or_equality_to_in.hpp"

#include "duckdb/optimizer/in_clause_rewriter.hpp"
#include "duckdb/parser/expression_map.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

namespace {

//! Types whose equality in a comparison is the equality the IN clause rewriter's join uses. Floating point is left
//! out: a join matches values by their hash, and -0.0 and NaN would need to hash like the values they equal.
bool SupportsOrToInRewrite(const LogicalType &type) {
	switch (type.InternalType()) {
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::INT128:
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::UINT128:
	case PhysicalType::VARCHAR:
		return true;
	default:
		return false;
	}
}

//! Whether the expression holds a constant anywhere in its tree; a lambda body is not a child of its function, so it
//! is walked explicitly
bool OrInputHasConstant(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		return true;
	}
	bool found = false;
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_LAMBDA) {
		auto &lambda = expr.Cast<BoundLambdaExpression>();
		found = OrInputHasConstant(*lambda.LambdaExpr());
		for (auto &capture : lambda.Captures()) {
			found = found || OrInputHasConstant(*capture);
		}
	}
	ExpressionIterator::EnumerateChildren(expr,
	                                      [&](const Expression &child) { found = found || OrInputHasConstant(child); });
	return found;
}

//! If the expression is input = constant (in either order), returns the input and the constant. Only inputs that
//! compute the same value whenever they compare equal are accepted: a constant inside one would compare by value, so
//! two inputs could look the same and differ (v + INTERVAL '1 month' and v + INTERVAL '30 days').
bool TryGetOrEquality(Expression &expr, optional_ptr<unique_ptr<Expression>> &input,
                      optional_ptr<unique_ptr<Expression>> &constant) {
	if (expr.GetExpressionType() != ExpressionType::COMPARE_EQUAL ||
	    expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &comparison = expr.Cast<BoundFunctionExpression>();
	auto &left = BoundComparisonExpression::LeftMutable(comparison);
	auto &right = BoundComparisonExpression::RightMutable(comparison);
	if (right->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		input = &left;
		constant = &right;
	} else if (left->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		input = &right;
		constant = &left;
	} else {
		return false;
	}
	auto &input_type = (*input)->GetReturnType();
	auto &value = (*constant)->Cast<BoundConstantExpression>().GetValue();
	return !value.IsNull() && value.type() == input_type && SupportsOrToInRewrite(input_type) &&
	       !(*input)->IsVolatile() && !OrInputHasConstant(**input);
}

} // namespace

OrEqualityToInRule::OrEqualityToInRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// match on an OR expression
	root = make_uniq<ExpressionMatcher>();
	root->expr_type = make_uniq<SpecificExpressionTypeMatcher>(ExpressionType::CONJUNCTION_OR);
}

unique_ptr<Expression> OrEqualityToInRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                 bool &changes_made, bool is_root) {
	auto &disjunction = bindings[0].get().Cast<BoundConjunctionExpression>();
	auto &children = disjunction.GetChildrenMutable();
	const idx_t threshold = InClauseRewriter::IN_CLAUSE_REWRITE_THRESHOLD - 1;
	if (children.size() < threshold) {
		return nullptr;
	}

	// group the equalities by their input; groups are kept in the order their first member appears in the OR
	struct Equality {
		idx_t child_idx;
		optional_ptr<unique_ptr<Expression>> constant;
	};
	expression_map_t<idx_t> group_of_input;
	vector<vector<Equality>> groups;
	vector<optional_ptr<unique_ptr<Expression>>> group_inputs;
	for (idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
		optional_ptr<unique_ptr<Expression>> input;
		optional_ptr<unique_ptr<Expression>> constant;
		if (!TryGetOrEquality(*children[child_idx], input, constant)) {
			continue;
		}
		auto entry = group_of_input.find(**input);
		idx_t group_idx;
		if (entry == group_of_input.end()) {
			group_idx = groups.size();
			group_of_input.emplace(**input, group_idx);
			groups.emplace_back();
			group_inputs.push_back(input);
		} else {
			group_idx = entry->second;
		}
		groups[group_idx].push_back(Equality {child_idx, constant});
	}

	// each group with enough equalities becomes one IN list in place of its first member
	vector<unique_ptr<Expression>> replacements(children.size());
	vector<bool> dropped(children.size(), false);
	bool rewrote_any = false;
	for (idx_t group_idx = 0; group_idx < groups.size(); group_idx++) {
		auto &group = groups[group_idx];
		// the IN list has the input as its first child, so it reaches the rewriter's threshold with one value less
		if (group.size() < threshold) {
			continue;
		}
		rewrote_any = true;
		auto in_list = make_uniq<BoundOperatorExpression>(ExpressionType::COMPARE_IN, LogicalType::BOOLEAN,
		                                                  (*group_inputs[group_idx])->Copy());
		for (auto &equality : group) {
			in_list->GetChildrenMutable().push_back(std::move(*equality.constant));
			dropped[equality.child_idx] = true;
		}
		dropped[group[0].child_idx] = false;
		replacements[group[0].child_idx] = std::move(in_list);
	}
	if (!rewrote_any) {
		return nullptr;
	}

	vector<unique_ptr<Expression>> new_children;
	for (idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
		if (dropped[child_idx]) {
			continue;
		}
		if (replacements[child_idx]) {
			new_children.push_back(std::move(replacements[child_idx]));
		} else {
			new_children.push_back(std::move(children[child_idx]));
		}
	}
	if (new_children.size() == 1) {
		return std::move(new_children[0]);
	}
	auto result = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR);
	for (auto &child : new_children) {
		result->GetChildrenMutable().push_back(std::move(child));
	}
	return std::move(result);
}

} // namespace duckdb
