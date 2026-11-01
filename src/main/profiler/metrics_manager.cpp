#include "duckdb/main/profiler/metrics_manager.hpp"
#include "duckdb/main/profiler/metrics.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/enums/optimizer_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/enum_util.hpp"

namespace duckdb {

namespace {

struct MetricDescriptor {
	const char *name;
	const char *metric_type;
	const char *description;
	const char *unit;
};

} // namespace

// clang-format off
#define DUCKDB_METRIC(METRIC) \
	{METRIC::Name, METRIC::TypeStr, METRIC::Description, METRIC::Unit}
#define FINAL_METRIC \
	{nullptr, nullptr, nullptr, nullptr}

static const MetricDescriptor internal_metrics[] = {
	DUCKDB_METRIC(MetricQueryCPUTime),
	DUCKDB_METRIC(MetricQuerySQL),
	DUCKDB_METRIC(MetricQueryTotalBytesScanned),
	DUCKDB_METRIC(MetricQueryTotalIntermediateRows),
	DUCKDB_METRIC(MetricQueryTotalIntermediateSizeBytes),
	DUCKDB_METRIC(MetricQueryTotalRowGroupsScanned),
	DUCKDB_METRIC(MetricQueryTotalRowGroupsToScan),
	DUCKDB_METRIC(MetricQueryTotalRowsScanned),
	DUCKDB_METRIC(MetricQueryTotalTime),
	DUCKDB_METRIC(MetricSystemBlockedThreadTime),
	DUCKDB_METRIC(MetricSystemPeakBufferMemory),
	DUCKDB_METRIC(MetricSystemPeakStreamingBufferSize),
	DUCKDB_METRIC(MetricSystemPeakTempDirSize),
	DUCKDB_METRIC(MetricSystemTotalBytesSpilled),
	DUCKDB_METRIC(MetricSystemTotalMemoryAllocated),
	DUCKDB_METRIC(MetricIOTotalBytesRead),
	DUCKDB_METRIC(MetricIOTotalBytesWritten),
	DUCKDB_METRIC(MetricIOTotalReadOperations),
	DUCKDB_METRIC(MetricIOTotalReadTime),
	DUCKDB_METRIC(MetricIOTotalWriteOperations),
	DUCKDB_METRIC(MetricIOTotalWriteTime),
	DUCKDB_METRIC(MetricStorageAttachLoadStorageLatency),
	DUCKDB_METRIC(MetricStorageAttachReplayWALLatency),
	DUCKDB_METRIC(MetricStorageCheckpointLatency),
	DUCKDB_METRIC(MetricStorageCommitLocalStorageLatency),
	DUCKDB_METRIC(MetricStorageTotalVacuumTime),
	DUCKDB_METRIC(MetricStorageWaitingToAttachLatency),
	DUCKDB_METRIC(MetricStorageWALReplayEntryCount),
	DUCKDB_METRIC(MetricStorageWriteToWALLatency),
	DUCKDB_METRIC(MetricOperatorBytesScanned),
	DUCKDB_METRIC(MetricOperatorCPUTime),
	DUCKDB_METRIC(MetricOperatorExtraInfo),
	DUCKDB_METRIC(MetricOperatorIntermediateRows),
	DUCKDB_METRIC(MetricOperatorIntermediateSizeBytes),
	DUCKDB_METRIC(MetricOperatorName),
	DUCKDB_METRIC(MetricOperatorRowGroupsScanned),
	DUCKDB_METRIC(MetricOperatorRowsScanned),
	DUCKDB_METRIC(MetricOperatorTiming),
	DUCKDB_METRIC(MetricOperatorTotalRowGroupsToScan),
	DUCKDB_METRIC(MetricOperatorType),
	DUCKDB_METRIC(MetricOptimizerTotalTime),
	DUCKDB_METRIC(MetricParserTotalTime),
	DUCKDB_METRIC(MetricPlannerBindingTime),
	DUCKDB_METRIC(MetricPlannerTotalTime),
	DUCKDB_METRIC(MetricPhysicalPlannerColumnBinding),
	DUCKDB_METRIC(MetricPhysicalPlannerCreatePlan),
	DUCKDB_METRIC(MetricPhysicalPlannerResolveTypes),
	DUCKDB_METRIC(MetricPhysicalPlannerTotalTime),
	FINAL_METRIC,
};
// clang-format on

#undef DUCKDB_METRIC
#undef FINAL_METRIC

MetricsManager &MetricsManager::Get(ClientContext &context) {
	return DatabaseInstance::GetDatabase(context).GetMetricsManager();
}

MetricsManager &MetricsManager::Get(DatabaseInstance &db) {
	return db.GetMetricsManager();
}

// The kind of one of the profiler's own metrics, as its type suggests: its timings are doubles, its counters uint64s.
static MetricKind FixedMetricKind(const string &metric_type) {
	if (metric_type == "double") {
		return MetricKind::TIMING;
	}
	if (metric_type == "uint64") {
		return MetricKind::COUNTER;
	}
	return MetricKind::VALUE;
}

namespace {

//! The metrics with a fixed index, built once: the profiler's own metrics in the order of their Index, then the
//! optimizers' timings.
struct FixedMetrics {
	FixedMetrics() {
		for (idx_t i = 0; internal_metrics[i].name; i++) {
			MetricInfo info;
			info.name = internal_metrics[i].name;
			info.metric_type = internal_metrics[i].metric_type;
			info.description = internal_metrics[i].description;
			info.unit = internal_metrics[i].unit;
			info.kind = FixedMetricKind(info.metric_type);
			metrics.push_back(std::move(info));
		}
		D_ASSERT(metrics.size() == CORE_METRIC_COUNT);
		for (auto &optimizer : ListAllOptimizers()) {
			auto type = OptimizerTypeFromString(optimizer);
			string opt_name = StringUtil::Lower(EnumUtil::ToString(type));
			MetricInfo info;
			info.name = "optimizer." + opt_name;
			info.metric_type = "double";
			info.description = "Time spent in the " + opt_name + " optimizer";
			info.unit = "seconds";
			info.kind = MetricKind::TIMING;
			auto type_value = static_cast<idx_t>(type);
			if (optimizer_indexes.size() <= type_value) {
				optimizer_indexes.resize(type_value + 1);
			}
			optimizer_indexes[type_value] = MetricIndex(metrics.size());
			metrics.push_back(std::move(info));
		}
		for (idx_t i = 0; i < metrics.size(); i++) {
			indexes[metrics[i].name] = i;
		}
	}

	vector<MetricInfo> metrics;
	unordered_map<string, idx_t> indexes;
	//! By the value of the OptimizerType
	vector<MetricIndex> optimizer_indexes;
};

const FixedMetrics &GetFixedMetrics() {
	static const FixedMetrics fixed_metrics;
	return fixed_metrics;
}

} // namespace

MetricsManager::MetricsManager() : indexed_metrics(make_shared_ptr<vector<MetricInfo>>(GetFixedMetrics().metrics)) {
}

idx_t MetricsManager::FixedMetricCount() {
	return GetFixedMetrics().metrics.size();
}

MetricIndex MetricsManager::OptimizerMetricIndex(OptimizerType type) {
	auto &optimizer_indexes = GetFixedMetrics().optimizer_indexes;
	auto type_value = static_cast<idx_t>(type);
	return type_value < optimizer_indexes.size() ? optimizer_indexes[type_value] : MetricIndex();
}

MetricIndex MetricsManager::FindFixedMetric(const string &name) {
	auto &indexes = GetFixedMetrics().indexes;
	auto entry = indexes.find(name);
	return entry == indexes.end() ? MetricIndex() : MetricIndex(entry->second);
}

shared_ptr<const vector<MetricInfo>> MetricsManager::GetIndexedMetrics() const {
	lock_guard<mutex> guard(registered_metrics_lock);
	return indexed_metrics;
}

idx_t MetricsManager::GetMetricCount() const {
	lock_guard<mutex> guard(registered_metrics_lock);
	idx_t count = sizeof(internal_metrics) / sizeof(MetricDescriptor) - 1;
	count += ListAllOptimizers().size();
	count += registered_metrics.size();
	return count;
}

vector<MetricInfo> MetricsManager::GetAllMetrics() const {
	lock_guard<mutex> guard(registered_metrics_lock);
	// the profiler's own metrics and the optimizers' timings, then the metrics extensions registered
	vector<MetricInfo> result = GetFixedMetrics().metrics;
	for (const auto &m : registered_metrics) {
		result.push_back(m);
	}
	return result;
}

MetricIndex MetricsManager::RegisterMetric(MetricInfo info) {
	if (FindFixedMetric(info.name).IsValid()) {
		return MetricIndex(); // the profiler's own metrics are already known, and are not set by extensions
	}
	lock_guard<mutex> guard(registered_metrics_lock);
	for (idx_t i = 0; i < registered_metrics.size(); i++) {
		if (registered_metrics[i].name == info.name) {
			return MetricIndex(FixedMetricCount() + i);
		}
	}
	registered_metrics.push_back(info);
	auto metrics = make_shared_ptr<vector<MetricInfo>>(*indexed_metrics);
	metrics->push_back(std::move(info));
	indexed_metrics = std::move(metrics);
	return MetricIndex(FixedMetricCount() + registered_metrics.size() - 1);
}

vector<MetricInfo> MetricsManager::GetRegisteredMetrics() const {
	lock_guard<mutex> guard(registered_metrics_lock);
	return registered_metrics;
}

bool MetricsManager::IsRegisteredMetric(const string &name) const {
	lock_guard<mutex> guard(registered_metrics_lock);
	for (const auto &existing : registered_metrics) {
		if (existing.name == name) {
			return true;
		}
	}
	return false;
}

} // namespace duckdb
