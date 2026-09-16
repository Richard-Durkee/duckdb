#include "duckdb/storage/buffer/buffer_pool_reservation.hpp"

#include "duckdb/storage/buffer/buffer_pool.hpp"

namespace duckdb {

BufferPoolReservation::BufferPoolReservation(MemoryTag tag, BufferPool &pool)
    : tag(tag), pool(pool), owner(BufferPool::CurrentOperator()) {
}

BufferPoolReservation::BufferPoolReservation(BufferPoolReservation &&src) noexcept : tag(src.tag), pool(src.pool) {
	size = src.size;
	owner = std::move(src.owner);
	src.size = 0;
}

BufferPoolReservation &BufferPoolReservation::operator=(BufferPoolReservation &&src) noexcept {
	pool.UpdateUsedMemory(tag, -UnsafeNumericCast<int64_t>(size));
	// PROTOTYPE: lock-free — release these bytes from the owning operator's counter before taking src's.
	if (owner) {
		owner->usage.fetch_sub(UnsafeNumericCast<int64_t>(size), std::memory_order_relaxed);
	}
	tag = src.tag;
	size = src.size;
	owner = std::move(src.owner);
	src.size = 0;
	return *this;
}

BufferPoolReservation::~BufferPoolReservation() {
	D_ASSERT(size == 0);
}

void BufferPoolReservation::Resize(idx_t new_size) {
	auto delta = UnsafeNumericCast<int64_t>(new_size) - UnsafeNumericCast<int64_t>(size);
	pool.UpdateUsedMemory(tag, delta);
	// PROTOTYPE: lock-free per-operator attribution — bump this reservation's owner counter directly.
	if (owner) {
		auto usage = owner->usage.fetch_add(delta, std::memory_order_relaxed) + delta;
		auto peak = owner->peak.load(std::memory_order_relaxed); // best-effort peak (racy, fine for a spike)
		if (usage > peak) {
			owner->peak.store(usage, std::memory_order_relaxed);
		}
	}
	size = new_size;
}

void BufferPoolReservation::Merge(BufferPoolReservation src) {
	// PROTOTYPE: the pool total already counts both reservations; per-operator, src's bytes were added to
	// src.owner at its own Resize. Re-attribute them to this owner (lock-free) so a later free decrements the
	// right counter. This closes the cross-owner Merge gap the string-label version had.
	if (src.owner != owner) {
		if (src.owner) {
			src.owner->usage.fetch_sub(UnsafeNumericCast<int64_t>(src.size), std::memory_order_relaxed);
		}
		if (owner) {
			owner->usage.fetch_add(UnsafeNumericCast<int64_t>(src.size), std::memory_order_relaxed);
		}
	}
	size += src.size;
	src.size = 0;
}

} // namespace duckdb
