#include "duckdb/storage/buffer/buffer_pool_reservation.hpp"

#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/buffer/memory_tracker.hpp"

namespace duckdb {

BufferPoolReservation::BufferPoolReservation(MemoryTag tag, BufferPool &pool) : tag(tag), pool(pool) {
}

BufferPoolReservation::BufferPoolReservation(BufferPoolReservation &&src) noexcept : tag(src.tag), pool(src.pool) {
	size = src.size;
	tracker = std::move(src.tracker);
	src.size = 0;
}

BufferPoolReservation &BufferPoolReservation::operator=(BufferPoolReservation &&src) noexcept {
	pool.UpdateUsedMemory(tag, -UnsafeNumericCast<int64_t>(size));
	if (tracker) {
		tracker->Update(-UnsafeNumericCast<int64_t>(size));
	}
	tag = src.tag;
	size = src.size;
	tracker = std::move(src.tracker);
	src.size = 0;
	return *this;
}

BufferPoolReservation::~BufferPoolReservation() {
	D_ASSERT(size == 0);
}

void BufferPoolReservation::Resize(idx_t new_size) {
	auto delta = UnsafeNumericCast<int64_t>(new_size) - UnsafeNumericCast<int64_t>(size);
	pool.UpdateUsedMemory(tag, delta);
	if (tracker) {
		tracker->Update(delta);
	}
	size = new_size;
}

void BufferPoolReservation::Merge(BufferPoolReservation src) {
	// src's bytes now belong to this reservation's tracker
	if (src.tracker != tracker) {
		src.SetTracker(tracker);
	}
	size += src.size;
	src.size = 0;
}

void BufferPoolReservation::SetTracker(shared_ptr<MemoryTracker> new_tracker) {
	if (new_tracker == tracker) {
		return;
	}
	auto bytes = UnsafeNumericCast<int64_t>(size);
	if (tracker) {
		tracker->Update(-bytes);
	}
	if (new_tracker) {
		new_tracker->Update(bytes);
	}
	tracker = std::move(new_tracker);
}

} // namespace duckdb
