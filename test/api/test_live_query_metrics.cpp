#include "catch.hpp"
#include "duckdb/main/client_context.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

TEST_CASE("Test live query metrics snapshot reports bytes read", "[api]") {
	auto path = TestCreatePath("live_query_metrics.db");
	DeleteDatabase(path);

	// persist a table to disk, then drop the database so the buffer cache is cold
	{
		DuckDB db(path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range AS i FROM range(1000000)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}

	// reopen: scanning the table now reads blocks from storage, even with profiling disabled
	{
		DuckDB db(path);
		Connection con(db);
		auto result = con.Query("SELECT sum(i) FROM t");
		REQUIRE_NO_FAIL(*result);

		auto metrics = con.context->GetLiveQueryMetrics();
		REQUIRE(metrics.bytes_read > 0);
		REQUIRE(metrics.read_operations > 0);
	}

	DeleteDatabase(path);
}
