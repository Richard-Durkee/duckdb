//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/window/window_rank_pruner.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/physical_operator_states.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"

#include <queue>

namespace duckdb {

//! A partition's tightest known boundary: the N-th best order key of a full heap, so N kept rows are at least this good
struct WindowRankPrunerBoundary {
	mutex lock;
	string key;
	//! Bumped on every tightening; 0 while there is no boundary yet
	atomic<idx_t> version {0};
};

//! What all threads and tasks of the query share: the tightest known boundary of each partition, and whether
//! filtering pays off
class WindowRankPrunerGlobalState : public GlobalSinkState {
public:
	//! Decide after this many rows, more when the heaps need longer to fill
	static constexpr idx_t MIN_WARMUP_ROWS = 32768;
	static constexpr idx_t MAX_WARMUP_ROWS = 262144;

	//! The partitions' boundaries by partition key, sharded by its hash so tasks rarely contend
	static constexpr idx_t SHARD_COUNT = 64;
	struct Shard {
		mutex lock;
		unordered_map<string, unique_ptr<WindowRankPrunerBoundary>> boundaries;
	};
	Shard shards[SHARD_COUNT];

	//! The rows filtered so far, and of those the ones that met a boundary and how many of them were kept
	atomic<idx_t> rows_seen {0};
	atomic<idx_t> bounded_seen {0};
	atomic<idx_t> bounded_kept {0};
	atomic<bool> decided {false};
	atomic<bool> disabled {false};

	//! Decides once whether filtering pays: only rows that meet a boundary can be dropped, so pruning is judged on
	//! those; if few rows meet one, the partitions are too small to prune
	void Update(idx_t seen, idx_t seen_bounded, idx_t kept_bounded, idx_t limit, idx_t partitions) {
		const auto total_seen = rows_seen.fetch_add(seen) + seen;
		const auto total_bounded = bounded_seen.fetch_add(seen_bounded) + seen_bounded;
		const auto total_kept = bounded_kept.fetch_add(kept_bounded) + kept_bounded;
		const auto fill_rows = 4 * limit * MaxValue<idx_t>(partitions, 1);
		const auto warmup = MinValue<idx_t>(MaxValue<idx_t>(MIN_WARMUP_ROWS, fill_rows), MAX_WARMUP_ROWS);
		if (decided || total_seen < warmup) {
			return;
		}
		decided = true;
		disabled = total_bounded * 4 < total_seen || total_kept * 2 > total_bounded;
	}
};

struct WindowRankPrunerPartition {
	//! The partition's sort key
	string partition_key;
	//! A max-heap of the N best (smallest) order keys of the partition kept by this task
	std::priority_queue<string> keys;
	//! The partition's shared boundary, and the copy of it this task last read
	optional_ptr<WindowRankPrunerBoundary> shared;
	idx_t version = 0;
	string boundary;
	bool has_boundary = false;
	//! Whether the heap got tighter than what this task published
	bool dirty = false;
};

class WindowRankPrunerLocalState : public LocalSinkState {
public:
	explicit WindowRankPrunerLocalState(ClientContext &context) : executor(context) {
	}

	//! Stop tracking new partitions beyond this many, and let their rows through
	static constexpr idx_t MAX_PARTITIONS = 16384;
	//! Hold at most this many order keys over all heaps; a heap that is not full yet stops growing beyond it
	static constexpr idx_t MAX_KEYS = 262144;
	idx_t key_count = 0;
	//! The partitions seen by this task, by the hash of their sort key
	unordered_map<hash_t, vector<unique_ptr<WindowRankPrunerPartition>>> partitions;
	idx_t partition_count = 0;
	vector<reference<WindowRankPrunerPartition>> dirty;
	//! Evaluates the ORDER BY expressions followed by the PARTITION BY expressions
	ExpressionExecutor executor;
	DataChunk key_inputs;
	DataChunk order_chunk;
	DataChunk partition_chunk;
	DataChunk keys;
	SelectionVector sel;
	DataChunk pruned;
	Vector hashes {LogicalType::HASH};
	vector<OrderModifiers> partition_modifiers;
};

//! Drops the rows of a rank() window filtered by rank <= N that cannot rank N or better: those of which N kept rows of
//! the same partition are strictly better. The window then sorts and ranks only the remaining rows.
class WindowRankPruner {
public:
	WindowRankPruner(const BoundWindowExpression &wexpr, idx_t limit);

	unique_ptr<WindowRankPrunerGlobalState> GetGlobalState() const;
	unique_ptr<WindowRankPrunerLocalState> GetLocalState(ClientContext &context) const;
	//! The rows of chunk that can still rank N or better
	DataChunk &Prune(WindowRankPrunerGlobalState &gstate, WindowRankPrunerLocalState &lstate, DataChunk &chunk) const;

	idx_t limit;
	vector<OrderModifiers> order_modifiers;
	idx_t partition_count;
	//! The ORDER BY expressions followed by the PARTITION BY expressions
	vector<unique_ptr<Expression>> key_expressions;
};

} // namespace duckdb
