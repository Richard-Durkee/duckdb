//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/likely.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#if __GNUC__
#define DUCKDB_BUILTIN_EXPECT(cond, expected_value) (__builtin_expect(cond, expected_value))
#else
#define DUCKDB_BUILTIN_EXPECT(cond, expected_value) (cond)
#endif

#define DUCKDB_LIKELY(...)   DUCKDB_BUILTIN_EXPECT((__VA_ARGS__), 1)
#define DUCKDB_UNLIKELY(...) DUCKDB_BUILTIN_EXPECT((__VA_ARGS__), 0)

//! Hints the CPU to start loading the cache line at ptr, for memory that is accessed soon in random order
#if __GNUC__
#define DUCKDB_PREFETCH(ptr) __builtin_prefetch(ptr)
#else
#define DUCKDB_PREFETCH(ptr)
#endif
