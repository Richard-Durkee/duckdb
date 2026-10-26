//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/window_rank_prefilter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class ClientContext;

//! Below a rank() window filtered by rk <= N, drops the rows that already have N strictly better rows in their
//! partition, so the window sorts only the rows that can be ranked N or better
class WindowRankPrefilter {
public:
	explicit WindowRankPrefilter(ClientContext &context);

	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);

	//! The largest N the prefilter is inserted for
	static constexpr int64_t MAX_LIMIT = 10000;

private:
	void Visit(unique_ptr<LogicalOperator> &op);
	bool TryInsert(LogicalOperator &filter_op);

private:
	ClientContext &context;
};

} // namespace duckdb
