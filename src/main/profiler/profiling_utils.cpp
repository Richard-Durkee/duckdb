#include "duckdb/main/profiler/profiling_utils.hpp"

#include <cstring>
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/profiler/metrics_manager.hpp"

namespace duckdb {

void QueryMetrics::InitializeIndexedMetrics(shared_ptr<const vector<MetricInfo>> metrics) {
	auto count = metrics->size();
	if (indexed_metric_capacity < count) {
		indexed_metric_values = make_unsafe_uniq_array<IndexedMetric>(count);
		indexed_metric_capacity = count;
	} else {
		for (idx_t i = 0; i < count; i++) {
			auto &metric = indexed_metric_values[i];
			metric.value.store(0, std::memory_order_relaxed);
			metric.start.store(NumericLimits<int64_t>::Maximum(), std::memory_order_relaxed);
			metric.end.store(NumericLimits<int64_t>::Minimum(), std::memory_order_relaxed);
			metric.set.store(false, std::memory_order_relaxed);
		}
	}
	indexed_metrics = std::move(metrics);
	indexed_metric_count.store(count, std::memory_order_relaxed);
}

void QueryMetrics::AddTimingToIndexedMetric(idx_t index, idx_t elapsed_ns, timestamp_t start_time,
                                            timestamp_t end_time) {
	if (index >= indexed_metric_count.load(std::memory_order_relaxed)) {
		return;
	}
	auto &metric = indexed_metric_values[index];
	metric.value.fetch_add(elapsed_ns, std::memory_order_relaxed);
	auto start = start_time.value;
	auto current_start = metric.start.load(std::memory_order_relaxed);
	while (start < current_start && !metric.start.compare_exchange_weak(current_start, start)) {
	}
	auto end = end_time.value;
	auto current_end = metric.end.load(std::memory_order_relaxed);
	while (end > current_end && !metric.end.compare_exchange_weak(current_end, end)) {
	}
	metric.set.store(true, std::memory_order_relaxed);
}

void QueryMetrics::SetIndexedMetric(idx_t index, const Value &value) {
	if (index >= indexed_metric_count.load(std::memory_order_relaxed) || value.IsNull()) {
		return;
	}
	uint64_t bits;
	if ((*indexed_metrics)[index].metric_type == "double") {
		auto double_value = value.GetValue<double>();
		memcpy(&bits, &double_value, sizeof(bits));
	} else {
		bits = value.GetValue<uint64_t>();
	}
	indexed_metric_values[index].value.store(bits, std::memory_order_relaxed);
	indexed_metric_values[index].set.store(true, std::memory_order_relaxed);
}

// The index of the metric `key`, when it has one and the running query has room for it.
static optional_idx IndexOfMetric(const string &key, idx_t indexed_metric_count) {
	auto index = MetricsManager::FindFixedMetric(key);
	if (!index.IsValid() || index.GetIndex() >= indexed_metric_count) {
		return optional_idx();
	}
	return index.GetIndex();
}

void QueryMetrics::UpdateMetric(const string &key, idx_t addition) {
	auto index = IndexOfMetric(key, indexed_metric_count.load(std::memory_order_relaxed));
	if (index.IsValid()) {
		AddToIndexedMetric(index.GetIndex(), addition);
		return;
	}
	annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
	string_timings[key].Add(addition);
}

void QueryMetrics::UpdateMetric(const string &key, idx_t addition, timestamp_t start_time, timestamp_t end_time) {
	auto index = IndexOfMetric(key, indexed_metric_count.load(std::memory_order_relaxed));
	if (index.IsValid()) {
		AddTimingToIndexedMetric(index.GetIndex(), addition, start_time, end_time);
		return;
	}
	annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
	string_timings[key].Add(addition, start_time, end_time);
}

void QueryMetrics::UpdateMetricCounter(const string &key, idx_t addition) {
	auto index = IndexOfMetric(key, indexed_metric_count.load(std::memory_order_relaxed));
	if (index.IsValid()) {
		AddToIndexedMetric(index.GetIndex(), addition);
		return;
	}
	annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
	string_counters[key] += addition;
}

// A metric with an index, as its kind and type report it.
static Value IndexedMetricValue(const MetricInfo &metric, uint64_t bits) {
	switch (metric.kind) {
	case MetricKind::COUNTER:
		return Value::UBIGINT(bits);
	case MetricKind::TIMING:
		return Value::DOUBLE(static_cast<double>(bits) / 1e9);
	case MetricKind::VALUE:
		if (metric.metric_type == "double") {
			double value;
			memcpy(&value, &bits, sizeof(value));
			return Value::DOUBLE(value);
		}
		return Value::UBIGINT(bits);
	}
	throw InternalException("Unrecognized MetricKind");
}

double QueryMetrics::GetStringMetricInSeconds(const string &key) const {
	auto index = IndexOfMetric(key, indexed_metric_count.load(std::memory_order_relaxed));
	if (index.IsValid()) {
		return static_cast<double>(indexed_metric_values[index.GetIndex()].value.load(std::memory_order_relaxed)) / 1e9;
	}
	annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
	auto it = string_timings.find(key);
	if (it == string_timings.end()) {
		return 0.0;
	}
	return static_cast<double>(it->second.duration) / 1e9;
}

idx_t QueryMetrics::GetStringCounter(const string &key) const {
	auto index = IndexOfMetric(key, indexed_metric_count.load(std::memory_order_relaxed));
	if (index.IsValid()) {
		return indexed_metric_values[index.GetIndex()].value.load(std::memory_order_relaxed);
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
	auto count = indexed_metric_count.load(std::memory_order_relaxed);
	for (idx_t i = 0; i < count; i++) {
		auto &metric = indexed_metric_values[i];
		if ((*indexed_metrics)[i].kind != MetricKind::TIMING || !metric.set.load(std::memory_order_relaxed)) {
			continue;
		}
		auto &timing = result[(*indexed_metrics)[i].name];
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
	auto count = indexed_metric_count.load(std::memory_order_relaxed);
	for (idx_t i = 0; i < count; i++) {
		auto &metric = indexed_metric_values[i];
		if ((*indexed_metrics)[i].kind == MetricKind::COUNTER && metric.set.load(std::memory_order_relaxed)) {
			result[(*indexed_metrics)[i].name] += metric.value.load(std::memory_order_relaxed);
		}
	}
	return result;
}

// Writes the metrics with an index from `begin` to `end` that the query updated into `info`.
void QueryMetrics::FinalizeIndexedMetrics(GatheredMetrics &info, idx_t begin, idx_t end) const {
	end = MinValue<idx_t>(end, indexed_metric_count.load(std::memory_order_relaxed));
	for (idx_t i = begin; i < end; i++) {
		auto &metric = indexed_metric_values[i];
		if (metric.set.load(std::memory_order_relaxed)) {
			auto &metric_info = (*indexed_metrics)[i];
			info.SetMetric(metric_info.name,
			               IndexedMetricValue(metric_info, metric.value.load(std::memory_order_relaxed)));
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
