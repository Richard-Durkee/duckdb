//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/buffer/buffer_pool_reservation.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/enums/memory_tag.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"

namespace duckdb {

enum class BlockState : uint8_t { BLOCK_UNLOADED = 0, BLOCK_LOADED = 1 };

// Forward declaration.
class BufferPool;

//! PROTOTYPE: a lock-free per-operator memory counter. One is created per sink operator; reservations made
//! while that sink runs hold a shared_ptr to it and bump `usage` directly (no map, no mutex on the hot path).
//! shared_ptr ownership makes lifetime safe: the counter outlives every reservation pointing at it.
struct OperatorMemoryCounter {
	explicit OperatorMemoryCounter(string label_p) : label(std::move(label_p)) {
	}
	atomic<int64_t> usage {0};
	atomic<int64_t> peak {0};
	string label;
};

struct BufferPoolReservation {
	MemoryTag tag;
	idx_t size {0};
	BufferPool &pool;
	//! PROTOTYPE: the operator counter this reservation's bytes are attributed to, captured at construction from
	//! BufferPool::CurrentOperator(). shared_ptr so the counter can't dangle if the reservation outlives the
	//! operator. Carried through moves so free decrements the same counter that alloc incremented.
	shared_ptr<OperatorMemoryCounter> owner;

	BufferPoolReservation(MemoryTag tag, BufferPool &pool);
	BufferPoolReservation(const BufferPoolReservation &) = delete;
	BufferPoolReservation &operator=(const BufferPoolReservation &) = delete;

	BufferPoolReservation(BufferPoolReservation &&) noexcept;
	BufferPoolReservation &operator=(BufferPoolReservation &&) noexcept;

	virtual ~BufferPoolReservation();

	void Resize(idx_t new_size);
	void Merge(BufferPoolReservation src);
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
