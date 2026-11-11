//===----------------------------------------------------------------------===//
//
//                         DuckDB
//
// duckdb/main/profiler/profiling_utils.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/metric_type.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/main/profiler/gathered_metrics.hpp"
#include "duckdb/main/profiler/metric_info.hpp"
#include "duckdb/main/profiler/profiling_node.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb_yyjson {
struct yyjson_mut_doc;
struct yyjson_mut_val;
} // namespace duckdb_yyjson

namespace duckdb {

class MetricsManager;
struct MetricsTimer;

//! How a metric with an index stores its value
enum class IndexedMetricStorage : uint8_t { UINT64, DOUBLE, BY_NAME };

//! A metric with an index, as the running query updated it, with atomics from any thread
struct IndexedMetric {
	//! A counter's count, a timing's nanoseconds, or a value's bits
	atomic<uint64_t> value {0};
	//! The earliest start and latest end of a timing, in microseconds since the epoch
	atomic<int64_t> start {NumericLimits<int64_t>::Maximum()};
	atomic<int64_t> end {NumericLimits<int64_t>::Minimum()};
	atomic<bool> set {false};
	//! Fixed when the slot is made, as the metric of an index never changes
	MetricKind kind = MetricKind::VALUE;
	IndexedMetricStorage storage = IndexedMetricStorage::UINT64;
};

//! The slots of the metrics with an index, for the metrics as they were when the slots were made
struct IndexedMetricSlots {
	explicit IndexedMetricSlots(shared_ptr<const IndexedMetrics> metrics);

	shared_ptr<const IndexedMetrics> metrics;
	unsafe_unique_array<IndexedMetric> values;
	idx_t count;

private:
	static IndexedMetricStorage StorageOf(const MetricInfo &metric);
};

// Top level query metrics
struct QueryMetrics {
public:
	QueryMetrics();
	~QueryMetrics();

	idx_t system_peak_buffer_memory;
	idx_t system_peak_streaming_buffer_size;
	idx_t system_peak_temp_dir_size;
	double blocked_thread_time;

	std::string query_sql;

	// Always-tracked byte counters (used by progress bar even when profiling is disabled)
	atomic<idx_t> bytes_read;
	atomic<idx_t> read_operations;
	atomic<idx_t> read_time_us;
	atomic<idx_t> bytes_written;
	atomic<idx_t> write_operations;
	atomic<idx_t> write_time_us;
	// Cumulative bytes written to the temporary (spill) directory
	atomic<idx_t> bytes_spilled;
	// Always tracked as well, so it can be read while the query runs: the compressed size of the column data scanned.
	// Not reported - query.total_bytes_scanned is summed over the operator tree, so that secure views hide theirs
	atomic<idx_t> bytes_scanned;
	// Thread-safe memory allocation counter (updated from allocator callbacks on any thread)
	atomic<idx_t> total_memory_allocated;

public:
	//! Add to a timing or a counter by name, through its index when it has one in the running query
	void UpdateMetric(const string &key, idx_t addition);
	void UpdateMetric(const string &key, idx_t addition, timestamp_t start_time, timestamp_t end_time);
	void UpdateMetricCounter(const string &key, idx_t addition);
	//! Set a value by name, through its index when it has one in the running query
	void SetMetricValue(const string &key, Value value);

	//! The index of the metric `key` in the running query; invalid when it has none
	MetricIndex FindIndexedMetric(const string &key) const;
	//! Add to the counter or timing of an index, without a lock. Does nothing for a VALUE.
	void AddToIndexedMetric(idx_t index, idx_t amount);
	void AddTimingToIndexedMetric(idx_t index, idx_t elapsed_ns, timestamp_t start_time, timestamp_t end_time);
	//! Set the VALUE of an index, cast to its type. Does nothing for a counter or timing, or a value that does not
	//! cast.
	void SetIndexedMetric(idx_t index, const Value &value);

	void UpdateBytesRead(idx_t n, idx_t elapsed_us) {
		bytes_read += n;
		read_operations++;
		read_time_us += elapsed_us;
	}

	void UpdateBytesWritten(idx_t n, idx_t elapsed_us) {
		bytes_written += n;
		write_operations++;
		write_time_us += elapsed_us;
	}

	void UpdateBytesSpilled(idx_t n) {
		bytes_spilled += n;
	}
	void UpdateBytesScanned(idx_t n) {
		bytes_scanned += n;
	}

	void UpdateTotalMemoryAllocated(idx_t n) {
		total_memory_allocated += n;
	}

	//! A timing by name, in seconds: 0 when the query has not timed it
	double GetStringMetricInSeconds(const string &key) const;
	//! A counter by name: 0 when the query has not counted it
	idx_t GetStringCounter(const string &key) const;

	idx_t GetBytesRead() const {
		return bytes_read.load();
	}

	idx_t GetReadOperations() const {
		return read_operations.load();
	}

	double GetReadTime() const {
		return static_cast<double>(read_time_us.load()) / 1e6;
	}

	idx_t GetBytesWritten() const {
		return bytes_written.load();
	}
	idx_t GetBytesScanned() const {
		return bytes_scanned.load();
	}

	idx_t GetWriteOperations() const {
		return write_operations.load();
	}

	double GetWriteTime() const {
		return static_cast<double>(write_time_us.load()) / 1e6;
	}

	idx_t GetBytesSpilled() const {
		return bytes_spilled.load();
	}

	idx_t GetTotalMemoryAllocated() const {
		return total_memory_allocated.load();
	}

	//! The timings and counters of the query, by name: a copy, as they may still be updated from other threads
	unordered_map<string, MetricTiming> GetMetricTimings() const;
	unordered_map<string, idx_t> GetMetricCounters() const;

	//! Reset the counters that are tracked without profiling; Reset keeps them, so they outlive their query.
	void ResetLiveMetrics() {
		bytes_read = 0;
		read_operations = 0;
		read_time_us = 0;
		bytes_written = 0;
		write_operations = 0;
		write_time_us = 0;
		bytes_spilled = 0;
		// bytes_scanned is not reset here, so it can be read after a failed query; StartQuery resets it
		total_memory_allocated = 0;
	}

	void Reset() {
		// ended before the metrics are reset, as the timer adds its time to them
		latency_timer.reset();
		{
			annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
			string_timings.clear();
			string_counters.clear();
			string_values.clear();
		}
		// the slots are kept: an update from another thread that comes late lands in them
		active_indexed_count.store(0, std::memory_order_release);

		query_sql = "";
		system_peak_buffer_memory = 0;
		system_peak_streaming_buffer_size = 0;
		system_peak_temp_dir_size = 0;
		blocked_thread_time = 0;
	}

	//! Make the slots of the metrics with an index ready for the query that starts. A metric registered while the
	//! query runs is reported from the next query on.
	void InitializeIndexedMetrics(const MetricsManager &manager);
	//! Write the values set with QueryProfiler::SetMetric and the metrics extensions registered into the given
	//! GatheredMetrics, before the metrics the profiler computes itself, which take precedence.
	void FinalizeMetricValues(GatheredMetrics &info);
	//! Write all query-level metrics into the given GatheredMetrics.
	void FinalizeMetrics(GatheredMetrics &info);
	//! The metrics backed by atomic counters, which can be read from any thread while the query runs
	profiler_metrics_t GetLiveMetrics() const;

private:
	//! The slot of an index in the running query, or nullptr
	optional_ptr<IndexedMetric> GetIndexedSlot(idx_t index) const;
	//! The metrics with an index from begin to end that the query updated
	void FinalizeIndexedMetrics(GatheredMetrics &info, idx_t begin, idx_t end) const;
	void SetStringValue(const string &key, Value value);
	static Value IndexedMetricValue(const IndexedMetric &metric);

	// Guards the metrics below that have no index in the running query, which threads update by name
	mutable annotated_mutex string_metrics_lock;
	unordered_map<string, MetricTiming> string_timings DUCKDB_GUARDED_BY(string_metrics_lock);
	unordered_map<string, idx_t> string_counters DUCKDB_GUARDED_BY(string_metrics_lock);
	// Values set by name, including "string" and "map" values of an index, kept until the metrics are gathered
	profiler_metrics_t string_values DUCKDB_GUARDED_BY(string_metrics_lock);
	// Every set of slots this profiler made, kept until it is destroyed, since a late update may still use one
	vector<unique_ptr<IndexedMetricSlots>> all_indexed_slots;
	// The slots of the running query, published before active_indexed_count, which is zero between queries
	atomic<IndexedMetricSlots *> indexed_slots {nullptr};
	atomic<idx_t> active_indexed_count {0};

public:
	// Declared last so it is destroyed first, as its destructor adds its time to the metrics above
	unique_ptr<MetricsTimer> latency_timer;
};

struct MetricsTimer {
public:
	MetricsTimer() : metric_name(""), is_active(false) {
	}
	MetricsTimer(QueryMetrics &query_metrics, string key, const bool is_active = true)
	    : query_metrics(query_metrics), metric_name(std::move(key)), is_active(is_active) {
		if (!is_active) {
			return;
		}
		start_time = Timestamp::GetCurrentTimestamp();
		profiler.Start();
	}
	//! A timer for a metric with an index, which it updates without looking up a name or taking a lock
	MetricsTimer(QueryMetrics &query_metrics, MetricIndex index, const bool is_active = true)
	    : query_metrics(query_metrics), metric_index(index), is_active(is_active && index.IsValid()) {
		if (!this->is_active) {
			return;
		}
		start_time = Timestamp::GetCurrentTimestamp();
		profiler.Start();
	}
	~MetricsTimer() {
		if (is_active && !Exception::UncaughtException()) {
			EndTimer();
		}
	}
	// disable copy constructors
	MetricsTimer(const MetricsTimer &other) = delete;
	MetricsTimer &operator=(const MetricsTimer &) = delete;
	//! enable move constructors
	MetricsTimer(MetricsTimer &&other) noexcept : is_active(false) {
		std::swap(query_metrics, other.query_metrics);
		std::swap(metric_name, other.metric_name);
		std::swap(metric_index, other.metric_index);
		std::swap(profiler, other.profiler);
		std::swap(start_time, other.start_time);
		std::swap(is_active, other.is_active);
	}
	MetricsTimer &operator=(MetricsTimer &&other) noexcept {
		std::swap(query_metrics, other.query_metrics);
		std::swap(metric_name, other.metric_name);
		std::swap(metric_index, other.metric_index);
		std::swap(profiler, other.profiler);
		std::swap(start_time, other.start_time);
		std::swap(is_active, other.is_active);
		return *this;
	}

	// Automatically called in the destructor.
	void EndTimer() {
		if (!is_active) {
			return;
		}
		is_active = false;
		profiler.End();
		auto end_time = Timestamp::GetCurrentTimestamp();
		if (metric_index.IsValid()) {
			query_metrics->AddTimingToIndexedMetric(metric_index.GetIndex(), profiler.ElapsedNanos(), start_time,
			                                        end_time);
		} else {
			query_metrics->UpdateMetric(metric_name, profiler.ElapsedNanos(), start_time, end_time);
		}
	}

	void Reset() {
		if (!is_active) {
			return;
		}
		profiler.Reset();
		is_active = false;
	}

private:
	optional_ptr<QueryMetrics> query_metrics;
	string metric_name;
	//! Valid for a timer of a metric with an index, which then has no metric_name
	MetricIndex metric_index;
	Profiler profiler;
	timestamp_t start_time = timestamp_t::epoch();
	bool is_active;
};

} // namespace duckdb
