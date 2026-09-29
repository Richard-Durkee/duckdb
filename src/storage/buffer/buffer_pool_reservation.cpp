#include "duckdb/storage/buffer/buffer_pool_reservation.hpp"

#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

namespace duckdb {

OperatorMemoryCounter::OperatorMemoryCounter(OperatorMemoryIdentity identity_p,
                                             optional_ptr<const PhysicalOperator> op_p)
    : identity(std::move(identity_p)), caches(make_uniq<array<Counters, CACHE_COUNT>>()), op(op_p) {
	for (auto &value : usage) {
		value = 0;
	}
	for (auto &cache : *caches) {
		for (auto &value : cache) {
			value = 0;
		}
	}
}

void OperatorMemoryCounter::UpdateGlobal(idx_t index, int64_t delta) {
	auto new_usage = usage[index].fetch_add(delta, std::memory_order_relaxed) + delta;
	if (index != TOTAL_INDEX) {
		return;
	}
	auto current_peak = peak.load(std::memory_order_relaxed);
	while (new_usage > current_peak && !peak.compare_exchange_weak(current_peak, new_usage)) {
	}
}

void OperatorMemoryCounter::Update(MemoryTag tag, int64_t delta) {
	auto tag_idx = static_cast<idx_t>(tag);
	if (static_cast<idx_t>(AbsValue(delta)) >= CACHE_THRESHOLD) {
		UpdateGlobal(tag_idx, delta);
		UpdateGlobal(TOTAL_INDEX, delta);
	} else {
		auto &cache = (*caches)[TaskScheduler::GetEstimatedCPUId() % CACHE_COUNT];
		for (auto index : {tag_idx, TOTAL_INDEX}) {
			auto cached = cache[index].fetch_add(delta, std::memory_order_relaxed) + delta;
			if (static_cast<idx_t>(AbsValue(cached)) >= CACHE_THRESHOLD) {
				UpdateGlobal(index, cache[index].exchange(0, std::memory_order_relaxed));
			}
		}
	}
	if (parent) {
		parent->Update(tag, delta);
	}
}

static idx_t ClampUsage(int64_t usage) {
	return usage > 0 ? static_cast<idx_t>(usage) : 0;
}

OperatorMemoryInformation OperatorMemoryCounter::GetInformation() const {
	// current usage includes the unflushed caches; the peak is only as exact as the cache threshold allows
	int64_t totals[MEMORY_TAG_COUNT + 1];
	for (idx_t index = 0; index <= TOTAL_INDEX; index++) {
		totals[index] = usage[index].load(std::memory_order_relaxed);
		for (auto &cache : *caches) {
			totals[index] += cache[index].load(std::memory_order_relaxed);
		}
	}
	OperatorMemoryInformation result;
	result.identity = identity;
	result.memory_usage_bytes = ClampUsage(totals[TOTAL_INDEX]);
	result.peak_memory_usage_bytes = ClampUsage(peak.load(std::memory_order_relaxed));
	for (idx_t tag_idx = 0; tag_idx < MEMORY_TAG_COUNT; tag_idx++) {
		result.memory_usage_bytes_per_tag[tag_idx] = ClampUsage(totals[tag_idx]);
	}
	return result;
}

shared_ptr<OperatorMemoryCounter> BufferPoolReservation::CurrentOwner() {
	auto current = BufferPool::CurrentOperator();
	return current ? current->shared_from_this() : nullptr;
}

BufferPoolReservation::BufferPoolReservation(MemoryTag tag, BufferPool &pool)
    : tag(tag), pool(pool), owner(CurrentOwner()) {
}

BufferPoolReservation::BufferPoolReservation(MemoryTag tag, BufferPool &pool, shared_ptr<OperatorMemoryCounter> owner)
    : tag(tag), pool(pool), owner(std::move(owner)) {
}

BufferPoolReservation::BufferPoolReservation(BufferPoolReservation &&src) noexcept : tag(src.tag), pool(src.pool) {
	size = src.size;
	owner = std::move(src.owner);
	src.size = 0;
}

BufferPoolReservation &BufferPoolReservation::operator=(BufferPoolReservation &&src) noexcept {
	pool.UpdateUsedMemory(tag, -UnsafeNumericCast<int64_t>(size));
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
	if (owner) {
		owner->Update(tag, delta);
	}
	size = new_size;
}

void BufferPoolReservation::Merge(BufferPoolReservation src) {
	// move src's bytes to this reservation's owner and tag, so the free releases them from the right counter
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

void BufferPoolReservation::SetOwner(shared_ptr<OperatorMemoryCounter> new_owner) {
	if (new_owner == owner) {
		return;
	}
	auto bytes = UnsafeNumericCast<int64_t>(size);
	if (owner) {
		owner->Update(tag, -bytes);
	}
	if (new_owner) {
		new_owner->Update(tag, bytes);
	}
	owner = std::move(new_owner);
}

} // namespace duckdb
