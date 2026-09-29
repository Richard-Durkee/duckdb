#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/buffer_manager.hpp"

using namespace duckdb; // NOLINT

// POC: data structures created through an operator's buffer manager must never outlive it

static const char *JOIN_QUERY = "SELECT count(*), sum(a.range) FROM range(300000) a JOIN range(300000) b ON a.range = b.range";

TEST_CASE("Memory facade: prepared statement re-executed many times", "[api][memory_facade]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("PRAGMA enable_profiling = 'no_output'"));
	auto prepared = con.Prepare(JOIN_QUERY);
	REQUIRE(!prepared->HasError());
	for (idx_t i = 0; i < 100; i++) {
		auto result = prepared->Execute();
		REQUIRE(CHECK_COLUMN(result, 0, {300000}));
	}
	// drained owner-bound allocators are recycled, so the pool stays bounded
	REQUIRE(BufferManager::GetBufferManager(*con.context).OwnedBufferAllocatorCount() < 10);
}

TEST_CASE("Memory facade: results outlive the prepared statement and later queries", "[api][memory_facade]") {
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

TEST_CASE("Memory facade: streaming join abandoned mid-probe, then plan destroyed", "[api][memory_facade]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("PRAGMA enable_profiling = 'no_output'"));
	{
		auto prepared = con.Prepare("SELECT a.range FROM range(1000000) a JOIN range(1000000) b ON a.range = b.range");
		auto streaming = prepared->PendingQuery();
		REQUIRE(!streaming->HasError());
		auto result = streaming->Execute();
		REQUIRE(result->Fetch());
		// the prepared statement (and its plan, holding the join's state) goes out of scope while streaming
	}
	REQUIRE_NO_FAIL(con.Query(JOIN_QUERY));
}

TEST_CASE("Memory facade: connection closed while a result is still held", "[api][memory_facade]") {
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

TEST_CASE("Memory facade: profiling toggled between executions of one plan", "[api][memory_facade]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto prepared = con.Prepare(JOIN_QUERY);
	for (idx_t i = 0; i < 20; i++) {
		REQUIRE_NO_FAIL(con.Query(i % 2 == 0 ? "PRAGMA enable_profiling = 'no_output'" : "PRAGMA disable_profiling"));
		auto result = prepared->Execute();
		REQUIRE(CHECK_COLUMN(result, 0, {300000}));
	}
}
