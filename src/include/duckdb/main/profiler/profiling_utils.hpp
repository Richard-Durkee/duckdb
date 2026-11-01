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

struct MetricsTimer;

//! A metric with an index, as the running query updated it. Lock-free: updated with atomics from any thread.
struct IndexedMetric {
	//! A counter's count, a timing's nanoseconds, or a value's bits
	atomic<uint64_t> value {0};
	//! The earliest start and latest end of a timing, in microseconds since the epoch
	atomic<int64_t> start {NumericLimits<int64_t>::Maximum()};
	atomic<int64_t> end {NumericLimits<int64_t>::Minimum()};
	atomic<bool> set {false};
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
	//! Add to a timing by name: a metric with an index, such as one of the profiler's own, is updated by its index
	void UpdateMetric(const string &key, idx_t addition);
	void UpdateMetric(const string &key, idx_t addition, timestamp_t start_time, timestamp_t end_time);
	//! Add to a counter by name: a metric with an index, such as one of the profiler's own, is updated by its index
	void UpdateMetricCounter(const string &key, idx_t addition);

	//! Add to a metric with an index: a counter's count, or a timing's nanoseconds. Lock-free.
	void AddToIndexedMetric(idx_t index, idx_t amount) {
		if (index < indexed_metric_count.load(std::memory_order_relaxed)) {
			indexed_metric_values[index].value.fetch_add(amount, std::memory_order_relaxed);
			indexed_metric_values[index].set.store(true, std::memory_order_relaxed);
		}
	}
	//! Add a timing's nanoseconds and its start and end to a metric with an index. Lock-free.
	void AddTimingToIndexedMetric(idx_t index, idx_t elapsed_ns, timestamp_t start_time, timestamp_t end_time);
	//! Set a metric with an index to a value of its type. Lock-free.
	void SetIndexedMetric(idx_t index, const Value &value);

	void SetMetricValue(const string &key, Value value) {
		annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
		string_values[key] = std::move(value);
	}

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
		// ended before taking the lock: the timer adds its time to string_timings
		latency_timer.reset();
		{
			annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
			string_timings.clear();
			string_counters.clear();
			string_values.clear();
		}
		// the room for the metrics with an index is kept, so that a late update from another thread lands in it
		indexed_metric_count.store(0, std::memory_order_relaxed);

		query_sql = "";
		system_peak_buffer_memory = 0;
		system_peak_streaming_buffer_size = 0;
		system_peak_temp_dir_size = 0;
		blocked_thread_time = 0;
	}

	//! Make room for the metrics with an index, for the query that starts: the profiler's own, the optimizers' timings
	//! and the metrics extensions registered. A metric registered while the query runs is only reported from the next
	//! query on, so the room never has to grow while it is updated.
	void InitializeIndexedMetrics(shared_ptr<const vector<MetricInfo>> metrics);
	//! Write the values set with QueryProfiler::SetMetric and the metrics extensions registered into the given
	//! GatheredMetrics, before the metrics the profiler computes itself, which take precedence.
	void FinalizeMetricValues(GatheredMetrics &info);
	//! Write the metrics with an index from begin to end that the query updated into the given GatheredMetrics
	void FinalizeIndexedMetrics(GatheredMetrics &info, idx_t begin, idx_t end) const;
	//! Write all query-level metrics into the given GatheredMetrics.
	void FinalizeMetrics(GatheredMetrics &info);
	//! The metrics backed by atomic counters, which can be read from any thread while the query runs
	profiler_metrics_t GetLiveMetrics() const;

	void Merge(const QueryMetrics &other) {
		auto other_timings = other.GetMetricTimings();
		auto other_counters = other.GetMetricCounters();
		for (const auto &entry : other_timings) {
			auto &timing = entry.second;
			if (timing.has_bounds) {
				UpdateMetric(entry.first, timing.duration, timing.start_time, timing.end_time);
			} else {
				UpdateMetric(entry.first, timing.duration);
			}
		}
		for (const auto &entry : other_counters) {
			UpdateMetricCounter(entry.first, entry.second);
		}
		bytes_read += other.bytes_read.load();
		read_operations += other.read_operations.load();
		read_time_us += other.read_time_us.load();
		bytes_written += other.bytes_written.load();
		write_operations += other.write_operations.load();
		write_time_us += other.write_time_us.load();
		bytes_spilled += other.bytes_spilled.load();
		bytes_scanned += other.bytes_scanned.load();
		total_memory_allocated += other.total_memory_allocated.load();
	}

private:
	// Guards the string-keyed metrics below, which operators, timers and extensions update from any thread
	mutable annotated_mutex string_metrics_lock;
	// String-keyed timings for optimizer, storage and phase timing metrics
	unordered_map<string, MetricTiming> string_timings DUCKDB_GUARDED_BY(string_metrics_lock);
	// String-keyed counters for storage counter metrics (e.g. "storage.wal_replay_entry_count")
	unordered_map<string, idx_t> string_counters DUCKDB_GUARDED_BY(string_metrics_lock);
	// Values set by QueryProfiler::SetMetric, kept until the query's metrics are gathered
	profiler_metrics_t string_values DUCKDB_GUARDED_BY(string_metrics_lock);
	// The metrics with an index, when the query started, and what the query updated them to. Only replaced between
	// queries, and the values only grow, so they are read and updated without a lock.
	shared_ptr<const vector<MetricInfo>> indexed_metrics;
	unsafe_unique_array<IndexedMetric> indexed_metric_values;
	idx_t indexed_metric_capacity = 0;
	atomic<idx_t> indexed_metric_count {0};

public:
	// Declared after string_timings and string_metrics_lock so it is destroyed first; its destructor writes to
	// string_timings.
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
