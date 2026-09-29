#include "duckdb/storage/buffer/memory_account.hpp"

namespace duckdb {

MemoryAccount::MemoryAccount(shared_ptr<MemoryAccount> parent_p) : peak(0), parent(std::move(parent_p)) {
}

void MemoryAccount::UpdatePeak(int64_t total) {
	auto current_peak = peak.load(std::memory_order_relaxed);
	while (total > current_peak && !peak.compare_exchange_weak(current_peak, total)) {
	}
}

void MemoryAccount::Update(MemoryTag tag, int64_t delta) {
	auto total = usage.UpdateUsedMemory(tag, delta);
	if (total != BufferPool::MemoryUsage::NO_TOTAL_CHANGE) {
		UpdatePeak(total);
	}
	if (parent) {
		parent->Update(tag, delta);
	}
}

idx_t MemoryAccount::GetMemoryUsage() {
	auto total = usage.GetUsedMemory(BufferPool::MemoryUsageCaches::FLUSH);
	UpdatePeak(UnsafeNumericCast<int64_t>(total));
	return total;
}

idx_t MemoryAccount::GetPeakMemoryUsage() const {
	auto result = peak.load(std::memory_order_relaxed);
	return result > 0 ? static_cast<idx_t>(result) : 0;
}

} // namespace duckdb
