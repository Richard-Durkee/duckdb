//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/profiler/metric_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/winapi.hpp"

namespace duckdb {

//! How the profiler updates a metric: SetMetric sets a VALUE, AddToMetric adds to a COUNTER or a TIMING
enum class MetricKind : uint8_t {
	//! The last value set, of any metric_type
	VALUE,
	//! The sum of the amounts added, a "uint64"
	COUNTER,
	//! The sum of the nanoseconds added, reported in seconds as a "double"
	TIMING
};

struct MetricInfo {
	string name;
	string metric_type; // "double" | "uint64" | "string" | "map"
	string description;
	string unit;
	MetricKind kind = MetricKind::VALUE;
};

//! The index of a metric, to update it without looking up its name. An index belongs to one database: an extension
//! loaded into several gets one for each. Updating an invalid index does nothing.
class MetricIndex {
public:
	MetricIndex() = default;
	explicit MetricIndex(idx_t index) : index(index) {
	}

	bool IsValid() const {
		return index.IsValid();
	}
	idx_t GetIndex() const {
		return index.GetIndex();
	}

private:
	optional_idx index;
};

//! The metrics with an index, in the order of their indexes, and their indexes by name. Never changed once built.
struct IndexedMetrics {
	vector<MetricInfo> metrics;
	unordered_map<string, idx_t> indexes;

	MetricIndex Find(const string &name) const {
		auto entry = indexes.find(name);
		return entry == indexes.end() ? MetricIndex() : MetricIndex(entry->second);
	}
};

} // namespace duckdb
