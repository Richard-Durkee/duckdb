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
#include "duckdb/common/optional_idx.hpp"
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
	MetricsManager() = default;

	DUCKDB_API static MetricsManager &Get(ClientContext &context);
	DUCKDB_API static MetricsManager &Get(DatabaseInstance &db);

	//! Returns the total number of known metrics (static + optimizer-generated + registered).
	DUCKDB_API idx_t GetMetricCount() const;

	//! Returns a snapshot of all known metrics.
	DUCKDB_API vector<MetricInfo> GetAllMetrics() const;

	//! Register an extension-defined metric. Silently ignores duplicates (same name) and the profiler's own metrics.
	DUCKDB_API void RegisterMetric(MetricInfo info);

	//! Whether an extension registered the metric `name`. The profiler's own metrics cannot be registered.
	DUCKDB_API bool IsRegisteredMetric(const string &name) const;

	//! The number of metrics with a fixed index: the profiler's own, then the optimizers' timings
	DUCKDB_API static idx_t FixedMetricCount();
	//! The metric with a fixed index
	DUCKDB_API static const MetricInfo &GetFixedMetric(idx_t index);
	//! Whether the metric with a fixed index is a timing, a "double", or a counter, a "uint64"
	DUCKDB_API static bool IsFixedTiming(idx_t index);
	DUCKDB_API static bool IsFixedCounter(idx_t index);
	//! The fixed index of the metric `name`; invalid when it is not one of the profiler's own
	DUCKDB_API static optional_idx FindFixedMetric(const string &name);
	//! The fixed index of an optimizer's timing
	DUCKDB_API static idx_t OptimizerMetricIndex(OptimizerType type);

private:
	struct FixedMetrics;
	static const FixedMetrics &GetFixedMetrics();

	mutable mutex registered_metrics_lock;
	vector<MetricInfo> registered_metrics;
};

} // namespace duckdb
