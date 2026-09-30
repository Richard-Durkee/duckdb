//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/buffer/buffer_pool_reservation.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/memory_tag.hpp"
#include "duckdb/common/shared_ptr.hpp"

namespace duckdb {

enum class BlockState : uint8_t { BLOCK_UNLOADED = 0, BLOCK_LOADED = 1 };

// Forward declaration.
class BufferPool;
class MemoryTracker;

struct BufferPoolReservation {
	MemoryTag tag;
	idx_t size {0};
	BufferPool &pool;
	//! The tracker this reservation is charged to, if any
	shared_ptr<MemoryTracker> tracker;

	BufferPoolReservation(MemoryTag tag, BufferPool &pool);
	BufferPoolReservation(const BufferPoolReservation &) = delete;
	BufferPoolReservation &operator=(const BufferPoolReservation &) = delete;

	BufferPoolReservation(BufferPoolReservation &&) noexcept;
	BufferPoolReservation &operator=(BufferPoolReservation &&) noexcept;

	virtual ~BufferPoolReservation();

	void Resize(idx_t new_size);
	void Merge(BufferPoolReservation src);
	//! Moves this reservation's bytes to `new_tracker`
	void SetTracker(shared_ptr<MemoryTracker> new_tracker);
};

struct TempBufferPoolReservation : BufferPoolReservation {
	TempBufferPoolReservation(MemoryTag tag, BufferPool &pool, idx_t size) : BufferPoolReservation(tag, pool) {
		Resize(size);
	}
	TempBufferPoolReservation(TempBufferPoolReservation &&) = default;
	~TempBufferPoolReservation() override {
		Resize(0);
	}
};

} // namespace duckdb
