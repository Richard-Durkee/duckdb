//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/group_key_encoding.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"

namespace duckdb {
class Optimizer;
class LogicalAggregate;
class ClientContext;
struct GroupKeyBudget;

//! The GroupKeyEncoding optimizer replaces VARCHAR group keys that come from a much smaller relation (a dimension
//! table) by dense integer ids. The ids are assigned where the dimension is scanned, so every string is interned once
//! per dimension row instead of being hashed and compared once per row of the large input. The aggregate (and every
//! grouping set level that is derived from it) then runs on the integer ids, and the strings are looked up again
//! right above the aggregate, one lookup per output group.
class GroupKeyEncoding : public LogicalOperatorVisitor {
public:
	//! Before the join order is known only grouping sets are encoded, afterwards only the other aggregates. The
	//! dictionaries of all aggregates of one query share the budget
	GroupKeyEncoding(Optimizer &optimizer, bool after_join_order, shared_ptr<GroupKeyBudget> budget);

	//! The memory that the dictionaries of one query may take together
	static shared_ptr<GroupKeyBudget> CreateBudget(ClientContext &context);

	void VisitOperator(unique_ptr<LogicalOperator> &op) override;

private:
	bool TryEncode(unique_ptr<LogicalOperator> &op);
	bool TryPullUpResolve(unique_ptr<LogicalOperator> &op);
	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr) override;

private:
	Optimizer &optimizer;
	bool after_join_order;
	shared_ptr<GroupKeyBudget> budget;
	column_binding_map_t<ColumnBinding> replacement_map;
};

} // namespace duckdb
