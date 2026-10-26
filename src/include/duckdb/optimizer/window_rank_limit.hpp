//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/window_rank_limit.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

class ClientContext;

//! Marks a rank() window filtered by rank <= N with N, so that it drops the rows that cannot rank N or better before
//! sorting them (see WindowRankPruner)
class WindowRankLimit {
public:
	WindowRankLimit(ClientContext &context, optional_ptr<column_binding_map_t<unique_ptr<BaseStatistics>>> stats);

	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);

	//! The largest N a window is marked for
	static constexpr int64_t MAX_LIMIT = 10000;

private:
	void Visit(unique_ptr<LogicalOperator> &op);
	bool TryMark(LogicalOperator &filter_op);
	//! Whether the statistics show too few rows per partition for any row to be dropped
	bool PartitionsTooSmall(LogicalOperator &input, const vector<unique_ptr<Expression>> &partitions, idx_t limit);

private:
	ClientContext &context;
	optional_ptr<column_binding_map_t<unique_ptr<BaseStatistics>>> stats;
};

} // namespace duckdb
