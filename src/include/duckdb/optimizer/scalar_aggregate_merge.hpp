//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/scalar_aggregate_merge.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class Optimizer;
struct ScalarAggregateBranch;

//! Merges the uncorrelated scalar subqueries of a cross product that aggregate the same table into one scan. Each
//! subquery is an ungrouped aggregate over a filtered scan; the merged branch scans the table once, keeps the rows any
//! subquery keeps, and gives each aggregate its own subquery's filter.
class ScalarAggregateMerge {
public:
	explicit ScalarAggregateMerge(Optimizer &optimizer);

	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);

private:
	void VisitOperator(unique_ptr<LogicalOperator> &op);
	void MergeCrossProduct(unique_ptr<LogicalOperator> &op);
	unique_ptr<LogicalOperator> MergeBranches(vector<ScalarAggregateBranch> &branches);

private:
	Optimizer &optimizer;
	ColumnBindingReplacer replacer;
};

} // namespace duckdb
