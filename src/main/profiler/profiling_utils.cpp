#include "duckdb/main/profiler/profiling_utils.hpp"

#include <cstring>
#include "duckdb/common/string_util.hpp"

namespace duckdb {

void QueryMetrics::InitializeRegisteredMetrics(vector<MetricInfo> metrics) {
	registered_metrics = std::move(metrics);
	registered_metric_count = registered_metrics.size();
	registered_metric_values = make_unsafe_uniq_array<atomic<uint64_t>>(registered_metric_count);
	registered_metric_set = make_unsafe_uniq_array<atomic<bool>>(registered_metric_count);
}

void QueryMetrics::SetRegisteredMetric(idx_t index, const Value &value) {
	if (index >= registered_metric_count || value.IsNull()) {
		return;
	}
	uint64_t bits;
	if (registered_metrics[index].metric_type == "double") {
		auto double_value = value.GetValue<double>();
		memcpy(&bits, &double_value, sizeof(bits));
	} else {
		bits = value.GetValue<uint64_t>();
	}
	registered_metric_values[index].store(bits, std::memory_order_relaxed);
	registered_metric_set[index].store(true, std::memory_order_relaxed);
}

// A metric an extension registered, as its kind and type report it.
static Value RegisteredMetricValue(const MetricInfo &metric, uint64_t bits) {
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

void QueryMetrics::FinalizeMetricValues(GatheredMetrics &info) {
	{
		annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
		for (const auto &[key, value] : string_values) {
			info.SetMetric(key, value);
		}
	}
	for (idx_t i = 0; i < registered_metric_count; i++) {
		if (registered_metric_set[i].load(std::memory_order_relaxed)) {
			auto bits = registered_metric_values[i].load(std::memory_order_relaxed);
			info.SetMetric(registered_metrics[i].name, RegisteredMetricValue(registered_metrics[i], bits));
		}
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
