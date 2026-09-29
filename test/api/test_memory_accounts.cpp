#include "catch.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/profiler/metrics.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "test_helpers.hpp"

using namespace duckdb; // NOLINT

static const char *JOIN_QUERY = "SELECT count(*) FROM range(300000) a JOIN range(300000) b ON a.range = b.range";

TEST_CASE("Live query metrics report the memory of a running profiled query", "[api][memory_accounts]") {
	DuckDB db(nullptr);
	Connection con(db);
	const string query = "SELECT a.range FROM range(1000000) a JOIN range(1000000) b ON a.range = b.range";

	// without profiling there are no memory accounts to report
	{
		auto streaming = con.SendQuery(query);
		REQUIRE_NO_FAIL(*streaming);
		REQUIRE(streaming->Fetch());
		auto metrics = con.context->GetLiveQueryMetrics();
		REQUIRE(metrics.count(MetricQueryMemoryUsage::Name) == 0);
	}

	// pause a streaming join mid-probe: its hash table is built and still held
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

TEST_CASE("Memory accounts: prepared statement re-executed many times", "[api][memory_accounts]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("PRAGMA enable_profiling = 'no_output'"));
	auto prepared = con.Prepare(JOIN_QUERY);
	REQUIRE(!prepared->HasError());
	for (idx_t i = 0; i < 100; i++) {
		auto result = prepared->Execute();
		REQUIRE(CHECK_COLUMN(result, 0, {300000}));
	}
	// drained account-bound allocators are recycled, so the pool stays bounded
	REQUIRE(BufferManager::GetBufferManager(*con.context).GetBufferAllocatorPoolSize() < 10);
}

TEST_CASE("Memory accounts: results outlive the prepared statement and later queries", "[api][memory_accounts]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("PRAGMA enable_profiling = 'no_output'"));
	unique_ptr<QueryResult> held;
	{
		auto prepared = con.Prepare("SELECT a.range FROM range(300000) a JOIN range(300000) b ON a.range = b.range "
		                            "ORDER BY 1 LIMIT 5");
		duckdb::vector<Value> no_values;
		held = prepared->Execute(no_values, false);
		REQUIRE_NO_FAIL(con.Query(JOIN_QUERY));
		REQUIRE_NO_FAIL(con.Query("SELECT count(*) FROM range(1000000) GROUP BY range % 7"));
	}
	REQUIRE(CHECK_COLUMN(held, 0, {0, 1, 2, 3, 4}));
}

TEST_CASE("Memory accounts: streaming join abandoned mid-probe, then plan destroyed", "[api][memory_accounts]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("PRAGMA enable_profiling = 'no_output'"));
	{
		auto prepared = con.Prepare("SELECT a.range FROM range(1000000) a JOIN range(1000000) b ON a.range = b.range");
		auto pending = prepared->PendingQuery();
		REQUIRE(!pending->HasError());
		auto result = pending->Execute();
		REQUIRE(result->Fetch());
	}
	REQUIRE_NO_FAIL(con.Query(JOIN_QUERY));
}

TEST_CASE("Memory accounts: connection closed while a result is still held", "[api][memory_accounts]") {
	DuckDB db(nullptr);
	unique_ptr<QueryResult> held;
	{
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("PRAGMA enable_profiling = 'no_output'"));
		held = con.Query("SELECT a.range FROM range(300000) a JOIN range(300000) b ON a.range = b.range "
		                 "ORDER BY 1 LIMIT 3");
	}
	REQUIRE(CHECK_COLUMN(held, 0, {0, 1, 2}));
}

TEST_CASE("Memory accounts: profiling toggled between executions of one plan", "[api][memory_accounts]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto prepared = con.Prepare(JOIN_QUERY);
	for (idx_t i = 0; i < 20; i++) {
		REQUIRE_NO_FAIL(con.Query(i % 2 == 0 ? "PRAGMA enable_profiling = 'no_output'" : "PRAGMA disable_profiling"));
		auto result = prepared->Execute();
		REQUIRE(CHECK_COLUMN(result, 0, {300000}));
	}
}
