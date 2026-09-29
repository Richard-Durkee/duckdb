//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/buffer/memory_account.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/buffer/buffer_pool.hpp"

namespace duckdb {

//! Buffer-managed memory attributed to a query or an operator. Uses the same per-CPU cached counters as the buffer
//! pool, and adds a peak. Accounts form a tree: charges roll up to the parent, so a query's account includes its
//! operators.
class MemoryAccount : public enable_shared_from_this<MemoryAccount> {
public:
	explicit MemoryAccount(shared_ptr<MemoryAccount> parent = nullptr);

	void Update(MemoryTag tag, int64_t delta);
	//! Current memory, including updates still cached per CPU
	idx_t GetMemoryUsage();
	//! Peak memory, exact to within the cache threshold of the buffer pool's own counters
	idx_t GetPeakMemoryUsage() const;

private:
	void UpdatePeak(int64_t total);

	BufferPool::MemoryUsage usage;
	atomic<int64_t> peak;
	shared_ptr<MemoryAccount> parent;
};

} // namespace duckdb
