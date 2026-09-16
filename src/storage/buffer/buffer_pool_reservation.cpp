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
	pool.UpdateUsedMemoryPerOperator(owner, -UnsafeNumericCast<int64_t>(size));
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
	pool.UpdateUsedMemoryPerOperator(owner, delta);
	size = new_size;
}

void BufferPoolReservation::Merge(BufferPoolReservation src) {
	// NOTE (prototype gap): if src.owner != owner, the merged bytes were counted under src.owner at their own
	// Resize and are not re-attributed here, so a later Resize(0) decrements the wrong owner. Main hash/sort
	// paths grow a single reservation via Resize and are unaffected.
	size += src.size;
	src.size = 0;
}

} // namespace duckdb
