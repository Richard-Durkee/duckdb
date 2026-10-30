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
#include "duckdb/common/winapi.hpp"

namespace duckdb {

//! How the profiler combines the updates of a metric an extension registered
enum class MetricKind : uint8_t {
	//! The last value set, a "uint64" or "double"
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

//! The index of a metric an extension registered, to update it without looking up its name. Updating an invalid index,
//! returned when the name is one of the profiler's own, does nothing.
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

} // namespace duckdb
