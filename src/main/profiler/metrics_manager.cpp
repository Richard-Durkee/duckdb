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
	idx_t index;
};

} // namespace

// clang-format off
#define DUCKDB_METRIC(METRIC) \
	{METRIC::Name, METRIC::TypeStr, METRIC::Description, METRIC::Unit, METRIC::Index}
#define FINAL_METRIC \
	{nullptr, nullptr, nullptr, nullptr, 0}

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

struct MetricsManager::FixedMetrics {
	FixedMetrics() {
		for (idx_t i = 0; internal_metrics[i].name; i++) {
			MetricInfo info;
			info.name = internal_metrics[i].name;
			info.metric_type = internal_metrics[i].metric_type;
			info.description = internal_metrics[i].description;
			info.unit = internal_metrics[i].unit;
			D_ASSERT(internal_metrics[i].index == i);
			metrics.push_back(std::move(info));
		}
		D_ASSERT(metrics.size() == CORE_METRIC_COUNT);
		// optimizer metrics are generated at runtime from the list of optimizers
		for (auto &optimizer : ListAllOptimizers()) {
			auto type = OptimizerTypeFromString(optimizer);
			string opt_name = StringUtil::Lower(EnumUtil::ToString(type));
			MetricInfo info;
			info.name = "optimizer." + opt_name;
			info.metric_type = "double";
			info.description = "Time spent in the " + opt_name + " optimizer";
			info.unit = "seconds";
			auto type_value = static_cast<idx_t>(type);
			if (optimizer_indexes.size() <= type_value) {
				optimizer_indexes.resize(type_value + 1);
			}
			optimizer_indexes[type_value] = metrics.size();
			metrics.push_back(std::move(info));
		}
		for (idx_t i = 0; i < metrics.size(); i++) {
			indexes[metrics[i].name] = i;
			is_timing.push_back(metrics[i].metric_type == "double");
			is_counter.push_back(metrics[i].metric_type == "uint64");
		}
	}

	vector<MetricInfo> metrics;
	unordered_map<string, idx_t> indexes;
	vector<bool> is_timing;
	vector<bool> is_counter;
	//! By the value of the OptimizerType
	vector<optional_idx> optimizer_indexes;
};

const MetricsManager::FixedMetrics &MetricsManager::GetFixedMetrics() {
	static const FixedMetrics fixed_metrics;
	return fixed_metrics;
}

idx_t MetricsManager::FixedMetricCount() {
	return GetFixedMetrics().metrics.size();
}

const MetricInfo &MetricsManager::GetFixedMetric(idx_t index) {
	return GetFixedMetrics().metrics[index];
}

bool MetricsManager::IsFixedTiming(idx_t index) {
	return GetFixedMetrics().is_timing[index];
}

bool MetricsManager::IsFixedCounter(idx_t index) {
	return GetFixedMetrics().is_counter[index];
}

optional_idx MetricsManager::FindFixedMetric(const string &name) {
	auto &indexes = GetFixedMetrics().indexes;
	auto entry = indexes.find(name);
	return entry == indexes.end() ? optional_idx() : optional_idx(entry->second);
}

idx_t MetricsManager::OptimizerMetricIndex(OptimizerType type) {
	auto &optimizer_indexes = GetFixedMetrics().optimizer_indexes;
	auto type_value = static_cast<idx_t>(type);
	if (type_value >= optimizer_indexes.size() || !optimizer_indexes[type_value].IsValid()) {
		throw InternalException("Optimizer %s has no metric: add it to the list of optimizers",
		                        EnumUtil::ToString(type));
	}
	return optimizer_indexes[type_value].GetIndex();
}

idx_t MetricsManager::GetMetricCount() const {
	lock_guard<mutex> guard(registered_metrics_lock);
	return FixedMetricCount() + registered_metrics.size();
}

vector<MetricInfo> MetricsManager::GetAllMetrics() const {
	vector<MetricInfo> result = GetFixedMetrics().metrics;
	lock_guard<mutex> guard(registered_metrics_lock);
	// extension-registered metrics
	for (const auto &m : registered_metrics) {
		result.push_back(m);
	}
	return result;
}

void MetricsManager::RegisterMetric(MetricInfo info) {
	if (FindFixedMetric(info.name).IsValid()) {
		return; // the profiler's own metrics are already known, and are not set by extensions
	}
	lock_guard<mutex> guard(registered_metrics_lock);
	for (const auto &existing : registered_metrics) {
		if (existing.name == info.name) {
			return; // silently ignore duplicate
		}
	}
	registered_metrics.push_back(std::move(info));
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
