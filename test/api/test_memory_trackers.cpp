#include "catch.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/profiler/metrics.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/buffer/memory_tracker.hpp"
#include "duckdb/storage/standard_buffer_manager.hpp"
#include "test_helpers.hpp"

using namespace duckdb; // NOLINT

TEST_CASE("Live query metrics report the memory of a running profiled query", "[api][memory_trackers]") {
	DuckDB db(nullptr);
	Connection con(db);
	const string query = "SELECT x FROM range(4000000) t(x) ORDER BY hash(x)";

	// without profiling there are no memory trackers to report
	{
		auto streaming = con.SendQuery(query);
		REQUIRE_NO_FAIL(*streaming);
		REQUIRE(streaming->Fetch());
		auto metrics = con.context->GetLiveQueryMetrics();
		REQUIRE(metrics.count(MetricQueryMemoryUsage::Name) == 0);
	}

	// pause a streaming sort after its first chunk: the sorted data is built and still held
	REQUIRE_NO_FAIL(con.Query("PRAGMA enable_profiling = 'no_output'"));
	auto streaming = con.SendQuery(query);
	REQUIRE_NO_FAIL(*streaming);
	REQUIRE(streaming->Fetch());
	auto metrics = con.context->GetLiveQueryMetrics();
	auto memory_usage = metrics[MetricQueryMemoryUsage::Name].GetValue<idx_t>();
	auto peak_memory = metrics[MetricQueryPeakMemory::Name].GetValue<idx_t>();
	REQUIRE(memory_usage > 0);
	REQUIRE(peak_memory >= memory_usage);
}

TEST_CASE("Memory trackers: tracker-bound buffer allocator charges allocate, reallocate and free",
          "[api][memory_trackers]") {
	DuckDB db(nullptr);
	auto &buffer_manager = dynamic_cast<StandardBufferManager &>(db.instance->GetBufferManager());
	auto tracker = make_shared_ptr<MemoryTracker>();
	auto &allocator = *buffer_manager.AcquireBufferAllocator(tracker);

	// sizes above the per-CPU cache threshold reach the tracker's total (and peak) directly
	const idx_t mib = 1024 * 1024;
	auto pointer = allocator.AllocateData(mib);
	REQUIRE(tracker->GetMemoryUsage() == mib);
	pointer = allocator.ReallocateData(pointer, mib, 5 * mib);
	REQUIRE(tracker->GetMemoryUsage() == 5 * mib);
	pointer = allocator.ReallocateData(pointer, 5 * mib, 2 * mib);
	REQUIRE(tracker->GetMemoryUsage() == 2 * mib);

	// released allocators are reused right away, but the allocation still releases the tracker it was charged to
	buffer_manager.ReleaseBufferAllocator(allocator);
	auto other_tracker = make_shared_ptr<MemoryTracker>();
	auto &reused = *buffer_manager.AcquireBufferAllocator(other_tracker);
	REQUIRE(&reused == &allocator);
	reused.FreeData(pointer, 2 * mib);
	REQUIRE(tracker->GetMemoryUsage() == 0);
	REQUIRE(tracker->GetPeakMemoryUsage() == 5 * mib);
	REQUIRE(other_tracker->GetMemoryUsage() == 0);
	buffer_manager.ReleaseBufferAllocator(reused);
	REQUIRE(buffer_manager.GetTrackerAllocatorCount() == 1);
}
