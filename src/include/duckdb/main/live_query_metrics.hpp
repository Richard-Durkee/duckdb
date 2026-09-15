//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/live_query_metrics.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

//! A snapshot of the always-tracked query-level counters. Every field is safe to read while a query is
//! still running, so it can be polled to observe progress even when profiling is disabled.
struct LiveQueryMetrics {
	idx_t bytes_read = 0;
	idx_t read_operations = 0;
	idx_t bytes_written = 0;
	idx_t write_operations = 0;
	idx_t total_memory_allocated = 0;
};

} // namespace duckdb
