#include "duckdb/storage/buffer/buffer_pool_reservation.hpp"

#include "duckdb/storage/buffer/buffer_pool.hpp"

namespace duckdb {

OperatorMemoryCounter::OperatorMemoryCounter(OperatorMemoryIdentity identity_p,
                                             optional_ptr<const PhysicalOperator> op_p)
    : identity(std::move(identity_p)), op(op_p) {
	for (auto &tag_usage : usage_per_tag) {
		tag_usage = 0;
	}
}

void OperatorMemoryCounter::Update(MemoryTag tag, int64_t delta) {
	usage_per_tag[static_cast<idx_t>(tag)].fetch_add(delta, std::memory_order_relaxed);
	auto new_usage = usage.fetch_add(delta, std::memory_order_relaxed) + delta;
	auto current_peak = peak.load(std::memory_order_relaxed);
	while (new_usage > current_peak && !peak.compare_exchange_weak(current_peak, new_usage)) {
	}
	if (parent) {
		parent->Update(tag, delta);
	}
}

static idx_t ClampUsage(int64_t usage) {
	return usage > 0 ? static_cast<idx_t>(usage) : 0;
}

OperatorMemoryInformation OperatorMemoryCounter::GetInformation() const {
	OperatorMemoryInformation result;
	result.identity = identity;
	result.memory_usage_bytes = ClampUsage(usage.load(std::memory_order_relaxed));
	result.peak_memory_usage_bytes = ClampUsage(peak.load(std::memory_order_relaxed));
	for (idx_t tag_idx = 0; tag_idx < MEMORY_TAG_COUNT; tag_idx++) {
		result.memory_usage_bytes_per_tag[tag_idx] = ClampUsage(usage_per_tag[tag_idx].load(std::memory_order_relaxed));
	}
	return result;
}

static shared_ptr<OperatorMemoryCounter> CurrentOwner() {
	auto current = BufferPool::CurrentOperator();
	return current ? current->shared_from_this() : nullptr;
}

BufferPoolReservation::BufferPoolReservation(MemoryTag tag, BufferPool &pool)
    : tag(tag), pool(pool), owner(CurrentOwner()) {
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
		owner->Update(tag, -UnsafeNumericCast<int64_t>(size));
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
		owner->Update(tag, delta);
	}
	size = new_size;
}

void BufferPoolReservation::Merge(BufferPoolReservation src) {
	// PROTOTYPE: the pool total already counts both reservations; per-operator, src's bytes were added to
	// src.owner at its own Resize. Re-attribute them to this owner (lock-free) so a later free decrements the
	// right counter (and tag). This closes the cross-owner Merge gap the string-label version had.
	if (src.owner != owner || src.tag != tag) {
		if (src.owner) {
			src.owner->Update(src.tag, -UnsafeNumericCast<int64_t>(src.size));
		}
		if (owner) {
			owner->Update(tag, UnsafeNumericCast<int64_t>(src.size));
		}
	}
	size += src.size;
	src.size = 0;
}

} // namespace duckdb
