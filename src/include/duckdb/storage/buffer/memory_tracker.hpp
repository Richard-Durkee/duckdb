//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/buffer/memory_tracker.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/array.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/shared_ptr.hpp"

namespace duckdb {

//! Buffer-managed memory attributed to a query or an operator: its current usage and its peak. Trackers form a tree:
//! charges roll up to the parent, so a query's tracker includes its operators.
class MemoryTracker : public enable_shared_from_this<MemoryTracker> {
public:
	explicit MemoryTracker(shared_ptr<MemoryTracker> parent = nullptr);

	void Update(int64_t delta);
	//! Current memory, including updates still cached per CPU
	idx_t GetMemoryUsage() const;
	//! Peak memory, exact to within CACHE_COUNT * CACHE_THRESHOLD bytes
	idx_t GetPeakMemoryUsage() const;

private:
	//! Same caching scheme as the buffer pool's memory counters: small updates accumulate in per-CPU slots and reach
	//! the shared counter once a slot exceeds the threshold, so threads rarely contend on a cache line
	static constexpr idx_t CACHE_COUNT = 64;
	static constexpr int64_t CACHE_THRESHOLD = 32 << 10;
	struct alignas(64) CacheSlot {
		atomic<int64_t> value {0};
	};

	void AddToTotal(int64_t delta);

	atomic<int64_t> usage {0};
	atomic<int64_t> peak {0};
	array<CacheSlot, CACHE_COUNT> caches;
	shared_ptr<MemoryTracker> parent;
};

} // namespace duckdb
