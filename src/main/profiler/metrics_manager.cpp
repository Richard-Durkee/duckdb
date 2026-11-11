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
	MetricKind kind;
};

} // namespace

// clang-format off
#define DUCKDB_METRIC(METRIC) \
	{METRIC::Name, METRIC::TypeStr, METRIC::Description, METRIC::Unit, METRIC::Index, METRIC::Kind}
#define FINAL_METRIC \
	{nullptr, nullptr, nullptr, nullptr, 0, MetricKind::VALUE}

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

//! The metrics with a fixed index, built once: the profiler's own metrics, then the optimizers' timings
struct MetricsManager::FixedMetrics {
	FixedMetrics() {
		for (idx_t i = 0; internal_metrics[i].name; i++) {
			D_ASSERT(internal_metrics[i].index == i);
			MetricInfo info;
			info.name = internal_metrics[i].name;
			info.metric_type = internal_metrics[i].metric_type;
			info.description = internal_metrics[i].description;
			info.unit = internal_metrics[i].unit;
			info.kind = internal_metrics[i].kind;
			metrics.metrics.push_back(std::move(info));
		}
		D_ASSERT(metrics.metrics.size() == CORE_METRIC_COUNT);
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
			optimizer_indexes[type_value] = MetricIndex(metrics.metrics.size());
			metrics.metrics.push_back(std::move(info));
		}
		for (idx_t i = 0; i < metrics.metrics.size(); i++) {
			metrics.indexes[metrics.metrics[i].name] = i;
		}
	}

	IndexedMetrics metrics;
	//! By the value of the OptimizerType
	vector<MetricIndex> optimizer_indexes;
};

const MetricsManager::FixedMetrics &MetricsManager::GetFixedMetrics() {
	static const FixedMetrics fixed_metrics;
	return fixed_metrics;
}

MetricsManager::MetricsManager() : indexed_metrics(make_shared_ptr<IndexedMetrics>(GetFixedMetrics().metrics)) {
	indexed_metric_count.store(indexed_metrics->metrics.size(), std::memory_order_release);
}

idx_t MetricsManager::FixedMetricCount() {
	return GetFixedMetrics().metrics.metrics.size();
}

MetricIndex MetricsManager::OptimizerMetricIndex(OptimizerType type) {
	auto &optimizer_indexes = GetFixedMetrics().optimizer_indexes;
	auto type_value = static_cast<idx_t>(type);
	return type_value < optimizer_indexes.size() ? optimizer_indexes[type_value] : MetricIndex();
}

MetricIndex MetricsManager::FindFixedMetric(const string &name) {
	return GetFixedMetrics().metrics.Find(name);
}

shared_ptr<const IndexedMetrics> MetricsManager::GetIndexedMetrics() const {
	lock_guard<mutex> guard(registered_metrics_lock);
	return indexed_metrics;
}

idx_t MetricsManager::GetMetricCount() const {
	return GetIndexedMetricCount();
}

vector<MetricInfo> MetricsManager::GetAllMetrics() const {
	return GetIndexedMetrics()->metrics;
}

MetricIndex MetricsManager::RegisterMetric(MetricInfo info) {
	if (FindFixedMetric(info.name).IsValid()) {
		return MetricIndex(); // the profiler's own metrics are already known, and are not set by extensions
	}
	auto &type = info.metric_type;
	if (type != "uint64" && type != "double" && type != "string" && type != "map") {
		throw InvalidInputException(
		    "Metric \"%s\" has type \"%s\": expected \"uint64\", \"double\", \"string\" or \"map\"", info.name, type);
	}
	if ((info.kind == MetricKind::COUNTER && type != "uint64") ||
	    (info.kind == MetricKind::TIMING && type != "double")) {
		throw InvalidInputException("Metric \"%s\" is a %s, which cannot have type \"%s\"", info.name,
		                            EnumUtil::ToString(info.kind), type);
	}
	lock_guard<mutex> guard(registered_metrics_lock);
	auto existing = indexed_metrics->Find(info.name);
	if (existing.IsValid()) {
		auto &known = indexed_metrics->metrics[existing.GetIndex()];
		if (known.metric_type != info.metric_type || known.kind != info.kind) {
			throw InvalidInputException("Metric \"%s\" is already registered as a %s of type \"%s\"", info.name,
			                            EnumUtil::ToString(known.kind), known.metric_type);
		}
		return existing;
	}
	auto metrics = make_shared_ptr<IndexedMetrics>(*indexed_metrics);
	auto index = metrics->metrics.size();
	metrics->indexes[info.name] = index;
	metrics->metrics.push_back(std::move(info));
	indexed_metrics = std::move(metrics);
	indexed_metric_count.store(index + 1, std::memory_order_release);
	return MetricIndex(index);
}

bool MetricsManager::IsRegisteredMetric(const string &name) const {
	auto index = GetIndexedMetrics()->Find(name);
	return index.IsValid() && index.GetIndex() >= FixedMetricCount();
}

} // namespace duckdb
