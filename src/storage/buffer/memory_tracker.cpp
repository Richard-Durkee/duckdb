#include "duckdb/storage/buffer/memory_tracker.hpp"

#include "duckdb/parallel/task_scheduler.hpp"

namespace duckdb {

MemoryTracker::MemoryTracker(shared_ptr<MemoryTracker> parent_p) : parent(std::move(parent_p)) {
}

void MemoryTracker::AddToTotal(int64_t delta) {
	auto total = usage.fetch_add(delta, std::memory_order_relaxed) + delta;
	auto current_peak = peak.load(std::memory_order_relaxed);
	while (total > current_peak && !peak.compare_exchange_weak(current_peak, total)) {
	}
}

void MemoryTracker::Update(int64_t delta) {
	if (AbsValue(delta) >= CACHE_THRESHOLD) {
		AddToTotal(delta);
	} else {
		auto &cache = caches[TaskScheduler::GetEstimatedCPUId() % CACHE_COUNT].value;
		auto cached = cache.fetch_add(delta, std::memory_order_relaxed) + delta;
		if (AbsValue(cached) >= CACHE_THRESHOLD) {
			AddToTotal(cache.exchange(0, std::memory_order_relaxed));
		}
	}
	if (parent) {
		parent->Update(delta);
	}
}

idx_t MemoryTracker::GetMemoryUsage() const {
	auto total = usage.load(std::memory_order_relaxed);
	for (auto &cache : caches) {
		total += cache.value.load(std::memory_order_relaxed);
	}
	return total > 0 ? static_cast<idx_t>(total) : 0;
}

idx_t MemoryTracker::GetPeakMemoryUsage() const {
	auto result = peak.load(std::memory_order_relaxed);
	return result > 0 ? static_cast<idx_t>(result) : 0;
}

} // namespace duckdb
