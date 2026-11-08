#include "duckdb/main/profiler/gathered_metrics.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/main/profiler/profiling_utils.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/logging/log_manager.hpp"

#include "yyjson.hpp"

using namespace duckdb_yyjson; // NOLINT

namespace duckdb {

static bool IsPrefixPattern(const string &pattern) {
	if (pattern.empty() || pattern.back() != '*') {
		return false;
	}
	for (idx_t i = 0; i + 1 < pattern.size(); i++) {
		if (pattern[i] == '*' || pattern[i] == '?' || pattern[i] == '[') {
			return false;
		}
	}
	return true;
}

GatheredMetrics::GatheredMetrics(const vector<string> &tracked_metrics) {
	InitTrackedMetrics(tracked_metrics);
	ResetMetrics();
}

void MetricPatterns::Add(const string &pattern) {
	if (pattern == "*") {
		all = true;
	} else if (!FileSystem::HasGlob(pattern)) {
		exact.insert(pattern);
	} else if (IsPrefixPattern(pattern)) {
		prefixes.insert(pattern.substr(0, pattern.size() - 1));
	} else {
		globs.push_back(pattern);
	}
}

bool MetricPatterns::Matches(const string &key) const {
	if (all) {
		return true;
	}
	if (exact.find(key) != exact.end()) {
		return true;
	}
	// walk backwards from the largest prefix <= key
	auto it = prefixes.upper_bound(key);
	while (it != prefixes.begin()) {
		--it;
		const auto &prefix = *it;
		if (prefix.size() <= key.size() && key.compare(0, prefix.size(), prefix) == 0) {
			return true;
		}
	}
	for (const auto &glob : globs) {
		if (Glob(key.c_str(), key.size(), glob.c_str(), glob.size())) {
			return true;
		}
	}
	return false;
}

void GatheredMetrics::InitTrackedMetrics(const vector<string> &patterns) {
	for (const auto &pattern : patterns) {
		if (StringUtil::StartsWith(pattern, "-")) {
			excluded.Add(pattern.substr(1));
		} else {
			tracked.Add(pattern);
		}
	}
	// only exclusions, e.g. ['-operator.*']: every other metric is tracked
	if (tracked.Empty() && !excluded.Empty()) {
		tracked.all = true;
	}
}

void GatheredMetrics::ResetMetrics() {
	metrics.clear();
}

bool GatheredMetrics::MetricIsTracked(const string &key) const {
	return tracked.Matches(key) && !excluded.Matches(key);
}

void GatheredMetrics::SetMetric(const string &key, Value new_value) {
	if (!MetricIsTracked(key)) {
		return;
	}
	metrics[key] = std::move(new_value);
}

void GatheredMetrics::SetMetric(const string &key, idx_t value) {
	if (!MetricIsTracked(key)) {
		return;
	}
	metrics[key] = Value::UBIGINT(value);
}

void GatheredMetrics::SetMetric(const string &key, double value) {
	if (!MetricIsTracked(key)) {
		return;
	}
	metrics[key] = Value::DOUBLE(value);
}

void GatheredMetrics::SetMetric(const string &key, const string &value) {
	if (!MetricIsTracked(key)) {
		return;
	}
	metrics[key] = Value(value);
}

void GatheredMetrics::WriteMetricsToLog(ClientContext &context,
                                        const unordered_map<string, MetricTiming> &timings) const {
	auto &logger = Logger::Get(context);
	if (logger.ShouldLog(MetricsLogType::NAME, MetricsLogType::LEVEL)) {
		for (const auto &entry : metrics) {
			auto timing = timings.find(entry.first);
			logger.WriteLog(MetricsLogType::NAME, MetricsLogType::LEVEL,
			                MetricsLogType::ConstructLogMessage(entry.first, entry.second,
			                                                    timing == timings.end() ? nullptr : &timing->second));
		}
	}
}

static QueryProfileResult &GetOrCreateObject(QueryProfileResult &parent, const string &key) {
	for (auto &child : parent.children) {
		if (child->kind == QueryProfileResultKind::OBJECT && child->key == key) {
			return *child;
		}
	}
	return parent.AddObject(key);
}

static void AddMetricToResult(QueryProfileResult &obj, const string &key, const Value &value) {
	auto dot_pos = key.find('.');
	if (dot_pos == string::npos) {
		obj.AddValue(key, value);
	} else {
		auto prefix = key.substr(0, dot_pos);
		auto suffix = key.substr(dot_pos + 1);
		auto &sub_obj = GetOrCreateObject(obj, prefix);
		AddMetricToResult(sub_obj, suffix, value);
	}
}

void GatheredMetrics::MetricsToProfileResult(QueryProfileResult &result) const {
	for (auto &entry : metrics) {
		AddMetricToResult(result, entry.first, entry.second);
	}
}

} // namespace duckdb
