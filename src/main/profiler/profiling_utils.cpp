#include "duckdb/main/profiler/profiling_utils.hpp"

#include <cstring>
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/profiler/metrics_manager.hpp"

namespace duckdb {

IndexedMetricStorage IndexedMetricSlots::StorageOf(const MetricInfo &metric) {
	if (metric.metric_type == "uint64") {
		return IndexedMetricStorage::UINT64;
	}
	if (metric.metric_type == "double") {
		return IndexedMetricStorage::DOUBLE;
	}
	return IndexedMetricStorage::BY_NAME;
}

IndexedMetricSlots::IndexedMetricSlots(shared_ptr<const IndexedMetrics> metrics_p)
    : metrics(std::move(metrics_p)), count(metrics->metrics.size()) {
	values = make_unsafe_uniq_array<IndexedMetric>(count);
	for (idx_t i = 0; i < count; i++) {
		values[i].kind = metrics->metrics[i].kind;
		values[i].storage = StorageOf(metrics->metrics[i]);
	}
}

void QueryMetrics::InitializeIndexedMetrics(const MetricsManager &manager) {
	auto slots = indexed_slots.load(std::memory_order_relaxed);
	if (!slots || slots->count != manager.GetIndexedMetricCount()) {
		// a metric was registered: new slots, while the old ones stay alive for an update that comes late
		all_indexed_slots.push_back(make_uniq<IndexedMetricSlots>(manager.GetIndexedMetrics()));
		slots = all_indexed_slots.back().get();
		indexed_slots.store(slots, std::memory_order_release);
	} else {
		for (idx_t i = 0; i < slots->count; i++) {
			auto &metric = slots->values[i];
			metric.value.store(0, std::memory_order_relaxed);
			metric.start.store(NumericLimits<int64_t>::Maximum(), std::memory_order_relaxed);
			metric.end.store(NumericLimits<int64_t>::Minimum(), std::memory_order_relaxed);
			metric.set.store(false, std::memory_order_relaxed);
		}
	}
	active_indexed_count.store(slots->count, std::memory_order_release);
}

optional_ptr<IndexedMetric> QueryMetrics::GetIndexedSlot(idx_t index) const {
	if (index >= active_indexed_count.load(std::memory_order_acquire)) {
		return nullptr;
	}
	// the slots are published before the count, and later slots have at least as many
	return &indexed_slots.load(std::memory_order_acquire)->values[index];
}

MetricIndex QueryMetrics::FindIndexedMetric(const string &key) const {
	if (active_indexed_count.load(std::memory_order_acquire) == 0) {
		return MetricIndex();
	}
	return indexed_slots.load(std::memory_order_acquire)->metrics->Find(key);
}

void QueryMetrics::AddToIndexedMetric(idx_t index, idx_t amount) {
	auto metric = GetIndexedSlot(index);
	if (!metric || metric->kind == MetricKind::VALUE) {
		return;
	}
	metric->value.fetch_add(amount, std::memory_order_relaxed);
	metric->set.store(true, std::memory_order_relaxed);
}

void QueryMetrics::AddTimingToIndexedMetric(idx_t index, idx_t elapsed_ns, timestamp_t start_time,
                                            timestamp_t end_time) {
	auto metric = GetIndexedSlot(index);
	if (!metric || metric->kind != MetricKind::TIMING) {
		return;
	}
	metric->value.fetch_add(elapsed_ns, std::memory_order_relaxed);
	auto start = start_time.value;
	auto current_start = metric->start.load(std::memory_order_relaxed);
	while (start < current_start && !metric->start.compare_exchange_weak(current_start, start)) {
	}
	auto end = end_time.value;
	auto current_end = metric->end.load(std::memory_order_relaxed);
	while (end > current_end && !metric->end.compare_exchange_weak(current_end, end)) {
	}
	metric->set.store(true, std::memory_order_relaxed);
}

void QueryMetrics::SetIndexedMetric(idx_t index, const Value &value) {
	auto metric = GetIndexedSlot(index);
	if (!metric || metric->kind != MetricKind::VALUE || value.IsNull()) {
		return;
	}
	uint64_t bits;
	switch (metric->storage) {
	case IndexedMetricStorage::UINT64: {
		auto cast = value.DefaultTryCastAs(LogicalType::UBIGINT);
		if (!cast || cast->IsNull()) {
			return;
		}
		bits = cast->GetValue<uint64_t>();
		break;
	}
	case IndexedMetricStorage::DOUBLE: {
		auto cast = value.DefaultTryCastAs(LogicalType::DOUBLE);
		if (!cast || cast->IsNull()) {
			return;
		}
		auto double_value = cast->GetValue<double>();
		memcpy(&bits, &double_value, sizeof(bits));
		break;
	}
	case IndexedMetricStorage::BY_NAME:
		// a "string" or "map" does not fit in the slot, so it is kept by name
		SetStringValue(indexed_slots.load(std::memory_order_acquire)->metrics->metrics[index].name, value);
		return;
	}
	metric->value.store(bits, std::memory_order_relaxed);
	metric->set.store(true, std::memory_order_relaxed);
}

void QueryMetrics::UpdateMetric(const string &key, idx_t addition) {
	auto index = FindIndexedMetric(key);
	if (index.IsValid()) {
		AddToIndexedMetric(index.GetIndex(), addition);
		return;
	}
	annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
	string_timings[key].Add(addition);
}

void QueryMetrics::UpdateMetric(const string &key, idx_t addition, timestamp_t start_time, timestamp_t end_time) {
	auto index = FindIndexedMetric(key);
	if (index.IsValid()) {
		AddTimingToIndexedMetric(index.GetIndex(), addition, start_time, end_time);
		return;
	}
	annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
	string_timings[key].Add(addition, start_time, end_time);
}

void QueryMetrics::UpdateMetricCounter(const string &key, idx_t addition) {
	auto index = FindIndexedMetric(key);
	if (index.IsValid()) {
		AddToIndexedMetric(index.GetIndex(), addition);
		return;
	}
	annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
	string_counters[key] += addition;
}

void QueryMetrics::SetMetricValue(const string &key, Value value) {
	auto index = FindIndexedMetric(key);
	if (index.IsValid()) {
		SetIndexedMetric(index.GetIndex(), value);
		return;
	}
	SetStringValue(key, std::move(value));
}

void QueryMetrics::SetStringValue(const string &key, Value value) {
	annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
	string_values[key] = std::move(value);
}

Value QueryMetrics::IndexedMetricValue(const IndexedMetric &metric) {
	auto bits = metric.value.load(std::memory_order_relaxed);
	if (metric.kind == MetricKind::TIMING) {
		return Value::DOUBLE(static_cast<double>(bits) / 1e9);
	}
	if (metric.storage == IndexedMetricStorage::DOUBLE) {
		double value;
		memcpy(&value, &bits, sizeof(value));
		return Value::DOUBLE(value);
	}
	return Value::UBIGINT(bits);
}

double QueryMetrics::GetStringMetricInSeconds(const string &key) const {
	auto index = FindIndexedMetric(key);
	if (index.IsValid()) {
		auto metric = GetIndexedSlot(index.GetIndex());
		if (!metric || metric->kind != MetricKind::TIMING) {
			return 0.0;
		}
		return static_cast<double>(metric->value.load(std::memory_order_relaxed)) / 1e9;
	}
	annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
	auto it = string_timings.find(key);
	if (it == string_timings.end()) {
		return 0.0;
	}
	return static_cast<double>(it->second.duration) / 1e9;
}

idx_t QueryMetrics::GetStringCounter(const string &key) const {
	auto index = FindIndexedMetric(key);
	if (index.IsValid()) {
		auto metric = GetIndexedSlot(index.GetIndex());
		if (!metric || metric->kind != MetricKind::COUNTER) {
			return 0;
		}
		return metric->value.load(std::memory_order_relaxed);
	}
	annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
	auto it = string_counters.find(key);
	if (it == string_counters.end()) {
		return 0;
	}
	return it->second;
}

unordered_map<string, MetricTiming> QueryMetrics::GetMetricTimings() const {
	unordered_map<string, MetricTiming> result;
	{
		annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
		result = string_timings;
	}
	auto count = active_indexed_count.load(std::memory_order_acquire);
	if (count == 0) {
		return result;
	}
	auto &slots = *indexed_slots.load(std::memory_order_acquire);
	for (idx_t i = 0; i < count; i++) {
		auto &metric = slots.values[i];
		if (metric.kind != MetricKind::TIMING || !metric.set.load(std::memory_order_relaxed)) {
			continue;
		}
		auto &timing = result[slots.metrics->metrics[i].name];
		auto duration = metric.value.load(std::memory_order_relaxed);
		auto start = metric.start.load(std::memory_order_relaxed);
		auto end = metric.end.load(std::memory_order_relaxed);
		if (start <= end) {
			timing.Add(duration, timestamp_t(start), timestamp_t(end));
		} else {
			timing.Add(duration);
		}
	}
	return result;
}

unordered_map<string, idx_t> QueryMetrics::GetMetricCounters() const {
	unordered_map<string, idx_t> result;
	{
		annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
		result = string_counters;
	}
	auto count = active_indexed_count.load(std::memory_order_acquire);
	if (count == 0) {
		return result;
	}
	auto &slots = *indexed_slots.load(std::memory_order_acquire);
	for (idx_t i = 0; i < count; i++) {
		auto &metric = slots.values[i];
		if (metric.kind == MetricKind::COUNTER && metric.set.load(std::memory_order_relaxed)) {
			result[slots.metrics->metrics[i].name] += metric.value.load(std::memory_order_relaxed);
		}
	}
	return result;
}

void QueryMetrics::FinalizeIndexedMetrics(GatheredMetrics &info, idx_t begin, idx_t end) const {
	auto count = active_indexed_count.load(std::memory_order_acquire);
	if (count == 0) {
		return;
	}
	auto &slots = *indexed_slots.load(std::memory_order_acquire);
	for (idx_t i = begin; i < MinValue<idx_t>(end, count); i++) {
		auto &metric = slots.values[i];
		// a "string" or "map" value is kept by name, with the values set by name
		if (metric.storage != IndexedMetricStorage::BY_NAME && metric.set.load(std::memory_order_relaxed)) {
			info.SetMetric(slots.metrics->metrics[i].name, IndexedMetricValue(metric));
		}
	}
}

void QueryMetrics::FinalizeMetricValues(GatheredMetrics &info) {
	{
		annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
		for (const auto &[key, value] : string_values) {
			info.SetMetric(key, value);
		}
	}
	// the metrics extensions registered
	FinalizeIndexedMetrics(info, MetricsManager::FixedMetricCount(), NumericLimits<idx_t>::Maximum());
}

void QueryMetrics::FinalizeMetrics(GatheredMetrics &info) {
	info.SetMetric<MetricQuerySQL>(query_sql);
	{
		annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
		for (const auto &[key, timing] : string_timings) {
			info.SetMetric(key, static_cast<double>(timing.duration) / 1e9);
		}
		for (const auto &[key, count] : string_counters) {
			info.SetMetric(key, count);
		}
	}
	// the profiler's own timings and counters, and the optimizers' timings
	FinalizeIndexedMetrics(info, 0, MetricsManager::FixedMetricCount());
	for (auto &entry : GetLiveMetrics()) {
		info.SetMetric(entry.first, std::move(entry.second));
	}
	info.SetMetric<MetricSystemBlockedThreadTime>(blocked_thread_time);
	info.SetMetric<MetricSystemPeakBufferMemory>(system_peak_buffer_memory);
	info.SetMetric<MetricSystemPeakStreamingBufferSize>(system_peak_streaming_buffer_size);
	info.SetMetric<MetricSystemPeakTempDirSize>(system_peak_temp_dir_size);
}

template <class METRIC>
static typename METRIC::METRIC_TYPE ReadLiveMetric(const QueryMetrics &metrics) {
	static_assert(sizeof(METRIC) == 0, "a metric marked \"live\" in metrics.json has no counter to read while the "
	                                   "query runs: add one here");
	return {};
}

template <>
uint64_t ReadLiveMetric<MetricIOTotalBytesRead>(const QueryMetrics &metrics) {
	return metrics.GetBytesRead();
}

template <>
uint64_t ReadLiveMetric<MetricIOTotalBytesWritten>(const QueryMetrics &metrics) {
	return metrics.GetBytesWritten();
}

template <>
uint64_t ReadLiveMetric<MetricSystemTotalMemoryAllocated>(const QueryMetrics &metrics) {
	return metrics.GetTotalMemoryAllocated();
}

template <>
uint64_t ReadLiveMetric<MetricIOTotalReadOperations>(const QueryMetrics &metrics) {
	return metrics.GetReadOperations();
}

template <>
double ReadLiveMetric<MetricIOTotalReadTime>(const QueryMetrics &metrics) {
	return metrics.GetReadTime();
}

template <>
uint64_t ReadLiveMetric<MetricIOTotalWriteOperations>(const QueryMetrics &metrics) {
	return metrics.GetWriteOperations();
}

template <>
double ReadLiveMetric<MetricIOTotalWriteTime>(const QueryMetrics &metrics) {
	return metrics.GetWriteTime();
}

template <>
uint64_t ReadLiveMetric<MetricSystemTotalBytesSpilled>(const QueryMetrics &metrics) {
	return metrics.GetBytesSpilled();
}

template <class... METRICS>
static profiler_metrics_t ReadLiveMetrics(const QueryMetrics &metrics, MetricList<METRICS...>) {
	profiler_metrics_t result;
	((result[METRICS::Name] = Value::CreateValue(ReadLiveMetric<METRICS>(metrics))), ...);
	return result;
}

profiler_metrics_t QueryMetrics::GetLiveMetrics() const {
	return ReadLiveMetrics(*this, LiveMetrics());
}

QueryMetrics::QueryMetrics()
    : bytes_read(0), read_operations(0), read_time_us(0), bytes_written(0), write_operations(0), write_time_us(0),
      bytes_spilled(0), bytes_scanned(0), total_memory_allocated(0) {
	Reset();
}

QueryMetrics::~QueryMetrics() = default;

} // namespace duckdb
