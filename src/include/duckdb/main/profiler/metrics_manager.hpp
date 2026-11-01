//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/profiler/metrics_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

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

	//! Register an extension-defined metric and return its index. Registering a name again returns the index it already
	//! has; the profiler's own metrics cannot be registered, and return an invalid index.
	DUCKDB_API MetricIndex RegisterMetric(MetricInfo info);

	//! A snapshot of the metrics extensions registered, in the order of their indexes.
	DUCKDB_API vector<MetricInfo> GetRegisteredMetrics() const;

	//! Every metric that has an index, in the order of their indexes: the profiler's own metrics, then the optimizers'
	//! timings, then the metrics extensions registered. Replaced, not changed, when an extension registers a metric.
	DUCKDB_API shared_ptr<const vector<MetricInfo>> GetIndexedMetrics() const;

	//! The number of metrics with a fixed index: the profiler's own metrics, then the optimizers' timings. The metrics
	//! extensions register come after them.
	DUCKDB_API static idx_t FixedMetricCount();
	//! The index of an optimizer's timing.
	DUCKDB_API static MetricIndex OptimizerMetricIndex(OptimizerType type);
	//! The index of one of the profiler's own metrics or an optimizer's timing, by name; invalid for any other name.
	DUCKDB_API static MetricIndex FindFixedMetric(const string &name);

	//! Whether an extension registered the metric `name`. The profiler's own metrics cannot be registered.
	DUCKDB_API bool IsRegisteredMetric(const string &name) const;

private:
	mutable mutex registered_metrics_lock;
	vector<MetricInfo> registered_metrics;
	//! The fixed metrics followed by registered_metrics, for GetIndexedMetrics
	shared_ptr<const vector<MetricInfo>> indexed_metrics;
};

} // namespace duckdb
