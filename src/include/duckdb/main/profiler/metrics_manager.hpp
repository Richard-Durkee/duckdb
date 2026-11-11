//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/profiler/metrics_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/optimizer_type.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/profiler/metric_info.hpp"

namespace duckdb {

class ClientContext;
class DatabaseInstance;

//! MetricsManager holds descriptors for every metric known to the system:
//! the statically compiled set (from src/common/metrics.json), optimizer
//! metrics generated at runtime from the OptimizerType enum, and any
//! additional metrics registered by extensions at runtime.
class MetricsManager {
public:
	MetricsManager();

	DUCKDB_API static MetricsManager &Get(ClientContext &context);
	DUCKDB_API static MetricsManager &Get(DatabaseInstance &db);

	//! Returns the total number of known metrics (static + optimizer-generated + registered).
	DUCKDB_API idx_t GetMetricCount() const;

	//! Returns a snapshot of all known metrics.
	DUCKDB_API vector<MetricInfo> GetAllMetrics() const;

	//! Register an extension-defined metric and return its index in this database. Registering a name again with the
	//! same type and kind returns the index it already has. The profiler's own metrics get an invalid index.
	//! Throws when the kind does not fit the type, or the name is already registered with another type or kind.
	DUCKDB_API MetricIndex RegisterMetric(MetricInfo info);

	//! Every metric with an index, in the order of their indexes: the profiler's own metrics, the optimizers' timings,
	//! then the metrics extensions registered. Replaced, not changed, when an extension registers a metric.
	DUCKDB_API shared_ptr<const IndexedMetrics> GetIndexedMetrics() const;
	//! The number of metrics GetIndexedMetrics returns, read without a lock
	idx_t GetIndexedMetricCount() const {
		return indexed_metric_count.load(std::memory_order_acquire);
	}

	//! The number of metrics with a fixed index: the profiler's own metrics, then the optimizers' timings
	DUCKDB_API static idx_t FixedMetricCount();
	//! The index of an optimizer's timing
	DUCKDB_API static MetricIndex OptimizerMetricIndex(OptimizerType type);
	//! The index of one of the profiler's own metrics or an optimizer's timing; invalid for any other name
	DUCKDB_API static MetricIndex FindFixedMetric(const string &name);

	//! Whether an extension registered the metric `name`. The profiler's own metrics cannot be registered.
	DUCKDB_API bool IsRegisteredMetric(const string &name) const;

private:
	struct FixedMetrics;
	static const FixedMetrics &GetFixedMetrics();

	mutable mutex registered_metrics_lock;
	shared_ptr<const IndexedMetrics> indexed_metrics;
	atomic<idx_t> indexed_metric_count {0};
};

} // namespace duckdb
