//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/scalar_aggregate_merger.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class Optimizer;

//! The ScalarAggregateMerger computes sibling ungrouped aggregates over the same table(s) in one pass.
//!
//! An uncorrelated scalar subquery that returns exactly one row, and a one-row derived table in a FROM list, are
//! planned as a cross product with the rest of the query, and each of them reads its tables on its own. Siblings of
//! the form [projection ->] ungrouped aggregate -> filters -> plain scan(s) of the same table(s) are rewritten into one
//! scan, filtered by the predicates every sibling shares and the OR of what each has beyond them, feeding one aggregate
//! that holds every sibling's aggregates, each with a FILTER (WHERE ...) of its own predicate. A projection then hands
//! every sibling's columns back under new bindings (TPC-DS q9, q28, q88 and q90).
//!
//! The merge trades scans for FILTERs, which pays off when the scan is the expensive part: siblings over a table
//! function (read_parquet, read_csv, ...), also under a view that only passes its columns on, or over a table of
//! another catalog are merged, siblings over a DuckDB table only when it has at least
//! scalar_aggregate_merge_threshold rows. Siblings whose rows are disjoint and that read mostly columns of their own
//! stay apart, as the merged scan would decode more than they do together.
class ScalarAggregateMerger {
public:
	explicit ScalarAggregateMerger(Optimizer &optimizer);

	//! Rewrites every eligible group of siblings in the plan
	void Optimize(unique_ptr<LogicalOperator> &plan);

public:
	struct Branch;
	struct MergeTarget;

private:
	void VisitOperator(unique_ptr<LogicalOperator> &op);
	//! Merges the eligible leaves of the cross-product tree rooted at `op`
	void MergeCrossProduct(unique_ptr<LogicalOperator> &op);
	//! Appends `leaf` to `branches` when it has the shape of a branch
	static void TryMatchBranch(unique_ptr<LogicalOperator> &leaf, vector<Branch> &branches);
	//! Whether the scans of `branch` cost enough for the merge to pay off: a table function (files, remote sources)
	//! or a table of at least scalar_aggregate_merge_threshold rows
	bool IsWorthMerging(const Branch &branch);
	//! Whether merging `group` reads no more column data than its separate scans do (see MergeBranches)
	static bool IsWidthAcceptable(const vector<idx_t> &group, vector<Branch> &branches,
	                              const vector<column_binding_map_t<ColumnBinding>> &remaps,
	                              const vector<unique_ptr<Expression>> &shared,
	                              const vector<vector<unique_ptr<Expression>>> &own);
	//! The merged leaf that replaces the branches of `group`, or null when the group cannot be merged
	unique_ptr<LogicalOperator> MergeBranches(vector<Branch> &branches, const vector<idx_t> &group);

private:
	Optimizer &optimizer;
	//! The bindings of the branches that were merged away, pointed at the merged projection's columns
	vector<ReplacementBinding> replacements;
};

} // namespace duckdb
