#include "duckdb/main/profiler/profiling_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/profiler/metrics_manager.hpp"

namespace duckdb {

void FixedMetric::Reset() {
	value.store(0, std::memory_order_relaxed);
	start.store(NumericLimits<int64_t>::Maximum(), std::memory_order_relaxed);
	end.store(NumericLimits<int64_t>::Minimum(), std::memory_order_relaxed);
	set.store(false, std::memory_order_relaxed);
}

bool FixedMetric::HasBounds() const {
	return start.load(std::memory_order_relaxed) != NumericLimits<int64_t>::Maximum();
}

void QueryMetrics::CheckFixedIndex(idx_t index) const {
	GetFixedSlot(index);
}

FixedMetric &QueryMetrics::GetFixedSlot(idx_t index) const {
	if (index >= fixed_metric_count) {
		throw InternalException("Metric index %llu is not one of the profiler's own", index);
	}
	return fixed_metrics[index];
}

optional_idx QueryMetrics::FindFixedMetric(const string &key, bool timing) const {
	auto index = MetricsManager::FindFixedMetric(key);
	if (!index.IsValid()) {
		return index;
	}
	auto &metric = fixed_metrics[index.GetIndex()];
	return (timing ? metric.is_timing : metric.is_counter) ? index : optional_idx();
}

void QueryMetrics::AddToFixedTiming(idx_t index, idx_t elapsed_ns, timestamp_t start_time, timestamp_t end_time) {
	auto &metric = GetFixedSlot(index);
	metric.value.fetch_add(elapsed_ns, std::memory_order_relaxed);
	auto start = start_time.value;
	auto current_start = metric.start.load(std::memory_order_relaxed);
	while (start < current_start && !metric.start.compare_exchange_weak(current_start, start, std::memory_order_relaxed,
	                                                                    std::memory_order_relaxed)) {
	}
	auto end = end_time.value;
	auto current_end = metric.end.load(std::memory_order_relaxed);
	while (end > current_end &&
	       !metric.end.compare_exchange_weak(current_end, end, std::memory_order_relaxed, std::memory_order_relaxed)) {
	}
	metric.set.store(true, std::memory_order_relaxed);
}

void QueryMetrics::AddToFixedCounter(idx_t index, idx_t addition) {
	auto &metric = GetFixedSlot(index);
	metric.value.fetch_add(addition, std::memory_order_relaxed);
	metric.set.store(true, std::memory_order_relaxed);
}

void QueryMetrics::UpdateMetric(const string &key, idx_t addition) {
	auto index = FindFixedMetric(key, true);
	if (index.IsValid()) {
		fixed_metrics[index.GetIndex()].value.fetch_add(addition, std::memory_order_relaxed);
		fixed_metrics[index.GetIndex()].set.store(true, std::memory_order_relaxed);
		return;
	}
	annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
	string_timings[key].Add(addition);
}

void QueryMetrics::UpdateMetric(const string &key, idx_t addition, timestamp_t start_time, timestamp_t end_time) {
	auto index = FindFixedMetric(key, true);
	if (index.IsValid()) {
		AddToFixedTiming(index.GetIndex(), addition, start_time, end_time);
		return;
	}
	annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
	string_timings[key].Add(addition, start_time, end_time);
}

void QueryMetrics::UpdateMetricCounter(const string &key, idx_t addition) {
	auto index = FindFixedMetric(key, false);
	if (index.IsValid()) {
		AddToFixedCounter(index.GetIndex(), addition);
		return;
	}
	annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
	string_counters[key] += addition;
}

double QueryMetrics::GetStringMetricInSeconds(const string &key) const {
	auto index = FindFixedMetric(key, true);
	if (index.IsValid()) {
		return static_cast<double>(fixed_metrics[index.GetIndex()].value.load(std::memory_order_relaxed)) / 1e9;
	}
	annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
	auto it = string_timings.find(key);
	if (it == string_timings.end()) {
		return 0.0;
	}
	return static_cast<double>(it->second.duration) / 1e9;
}

idx_t QueryMetrics::GetStringCounter(const string &key) const {
	auto index = FindFixedMetric(key, false);
	if (index.IsValid()) {
		return fixed_metrics[index.GetIndex()].value.load(std::memory_order_relaxed);
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
	for (idx_t i = 0; i < fixed_metric_count; i++) {
		auto &metric = fixed_metrics[i];
		if (!metric.is_timing || !metric.set.load(std::memory_order_relaxed)) {
			continue;
		}
		auto &timing = result[MetricsManager::GetFixedMetric(i).name];
		auto duration = metric.value.load(std::memory_order_relaxed);
		auto start = metric.start.load(std::memory_order_relaxed);
		auto end = metric.end.load(std::memory_order_relaxed);
		if (metric.HasBounds()) {
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
	for (idx_t i = 0; i < fixed_metric_count; i++) {
		auto &metric = fixed_metrics[i];
		if (metric.is_counter && metric.set.load(std::memory_order_relaxed)) {
			result[MetricsManager::GetFixedMetric(i).name] += metric.value.load(std::memory_order_relaxed);
		}
	}
	return result;
}

void QueryMetrics::MergeFixedMetrics(const QueryMetrics &other) {
	for (idx_t i = 0; i < fixed_metric_count; i++) {
		auto &metric = other.fixed_metrics[i];
		if (!metric.set.load(std::memory_order_relaxed)) {
			continue;
		}
		auto value = metric.value.load(std::memory_order_relaxed);
		if (metric.is_timing && metric.HasBounds()) {
			AddToFixedTiming(i, value, timestamp_t(metric.start.load(std::memory_order_relaxed)),
			                 timestamp_t(metric.end.load(std::memory_order_relaxed)));
		} else {
			fixed_metrics[i].value.fetch_add(value, std::memory_order_relaxed);
			fixed_metrics[i].set.store(true, std::memory_order_relaxed);
		}
	}
}

void QueryMetrics::FinalizeMetricValues(GatheredMetrics &info) {
	annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
	for (const auto &[key, value] : string_values) {
		info.SetMetric(key, value);
	}
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
	// the profiler's own timings and counters
	for (idx_t i = 0; i < fixed_metric_count; i++) {
		auto &metric = fixed_metrics[i];
		if (!metric.set.load(std::memory_order_relaxed)) {
			continue;
		}
		auto &name = MetricsManager::GetFixedMetric(i).name;
		auto value = metric.value.load(std::memory_order_relaxed);
		if (metric.is_timing) {
			info.SetMetric(name, static_cast<double>(value) / 1e9);
		} else {
			info.SetMetric(name, value);
		}
	}
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
      bytes_spilled(0), bytes_scanned(0), total_memory_allocated(0),
      fixed_metrics(make_uniq_array<FixedMetric>(MetricsManager::FixedMetricCount())),
      fixed_metric_count(MetricsManager::FixedMetricCount()) {
	for (idx_t i = 0; i < fixed_metric_count; i++) {
		fixed_metrics[i].is_timing = MetricsManager::IsFixedTiming(i);
		fixed_metrics[i].is_counter = MetricsManager::IsFixedCounter(i);
	}
	Reset();
}

QueryMetrics::~QueryMetrics() = default;

} // namespace duckdb
