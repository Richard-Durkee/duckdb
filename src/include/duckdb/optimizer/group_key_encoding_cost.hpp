//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/group_key_encoding_cost.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/string.hpp"

namespace duckdb {
class ClientContext;
class LogicalOperator;

//! What the cost model of the group key encoding knows about one encoded group column
struct GroupKeyEncodingCostInput {
	//! Estimated before the join order (grouping sets): the input rows are only an upper bound
	bool early = false;
	//! The rows that reach the aggregate
	idx_t input_rows = 0;
	//! The rows of the relation that is interned (the dictionary is built once per row)
	idx_t dimension_rows = 0;
	//! The estimated number of output rows of the aggregate (an upper bound of the groups)
	idx_t group_rows = 0;
	//! The number of grouping sets of the aggregate (one for a plain GROUP BY)
	idx_t grouping_sets = 1;
	//! Threads that execute the query
	idx_t threads = 1;
};

struct GroupKeyEncodingDecision {
	bool encode = false;
	//! The work that the integer ids save, and the work that building and resolving the dictionary costs (CPU ns)
	double benefit_cpu_ns = 0;
	double cost_cpu_ns = 0;
	//! The same on the critical path of the query (ns), assuming the scans scale with the number of threads
	double benefit_wall_ns = 0;
	double cost_wall_ns = 0;
};

//! Decides whether the group key of a relation is worth interning:
//!   benefit = input rows * (1 + CASCADE * (grouping sets - 1)) * B + groups * S   (string hash, compare, copy; a
//!   string per group) cost    = dimension rows * C + groups * R                                     (intern and
//!   resolve)
//! Encode if benefit >= K * cost on the CPU and on the critical path, and the saving is above a floor.
GroupKeyEncodingDecision DecideGroupKeyEncoding(const GroupKeyEncodingCostInput &input);

//! A key that the model declined but whose dictionary costs next to nothing next to what the other keys of the
//! aggregate save: encode it as well, so that no string is left in the hash table
bool IsFreeRider(const GroupKeyEncodingDecision &decision, double encoded_benefit_cpu_ns);

//! The number of rows that reach an operator, bounded by what its children produce. The estimate of a join is capped by
//! its larger input, so a join that was reduced after the join order (a pre-aggregation below it) is not credited with
//! the rows that no longer exist.
idx_t BoundedInputRows(ClientContext &context, LogicalOperator &op);

//! The rows that a join returns when one of its sides is an aggregate that was grouped by the join key (the result of a
//! partial aggregate pushdown) and the other side has this many rows
idx_t PreAggregatedJoinRows(idx_t other_side_rows);

} // namespace duckdb
