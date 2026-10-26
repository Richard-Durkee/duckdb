//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/window_rank_prefilter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

class ClientContext;

//! Below a rank() window filtered by rk <= N, drops the rows that already have N strictly better rows in their
//! partition, so the window sorts only the rows that can be ranked N or better
class WindowRankPrefilter {
public:
	WindowRankPrefilter(ClientContext &context, optional_ptr<column_binding_map_t<unique_ptr<BaseStatistics>>> stats);

	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);

	//! The largest N the prefilter is inserted for
	static constexpr int64_t MAX_LIMIT = 10000;

private:
	void Visit(unique_ptr<LogicalOperator> &op);
	bool TryInsert(LogicalOperator &filter_op);
	//! Whether the statistics show too few rows per partition for any row to be dropped
	bool PartitionsTooSmall(LogicalOperator &input, const vector<unique_ptr<Expression>> &partitions, idx_t limit);

private:
	ClientContext &context;
	optional_ptr<column_binding_map_t<unique_ptr<BaseStatistics>>> stats;
};

} // namespace duckdb
