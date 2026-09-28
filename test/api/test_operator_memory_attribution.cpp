#include "catch.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "test_helpers.hpp"

using namespace duckdb; // NOLINT

TEST_CASE("Test reserved memory is not charged to the current operator", "[api][operator_memory]") {
	DuckDB db;
	Connection con(db);
	auto &buffer_manager = BufferManager::GetBufferManager(*con.context);
	OperatorMemoryIdentity identity;
	identity.operator_name = "TEST_OPERATOR";
	auto counter = buffer_manager.GetBufferPool().RegisterOperatorCounter(identity, nullptr);

	// FreeReservedMemory has no owner to release, so reserving must not charge the operator
	const idx_t reservation_size = 4ULL * 1024ULL * 1024ULL;
	{
		OperatorMemoryScope scope(counter);
		buffer_manager.ReserveMemory(reservation_size);
	}
	buffer_manager.FreeReservedMemory(reservation_size);
	REQUIRE(counter->usage.load() == 0);
	REQUIRE(counter->peak.load() == 0);
}

TEST_CASE("Test duckdb_operator_memory lists the operators of running queries only", "[api][operator_memory]") {
	DuckDB db;
	Connection con(db);
	Connection inspector(db);
	REQUIRE_NO_FAIL(con.Query("SET threads = 1"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA enable_profiling = 'no_output'"));

	// with a single thread the query only progresses through ExecuteTask, so it can be inspected mid-flight
	auto pending = con.PendingQuery("SELECT count(*) FROM range(1000000) a JOIN range(1000000) b ON a.range = b.range");
	REQUIRE(!pending->HasError());
	const auto connection_id = to_string(con.context->GetConnectionId());
	bool saw_join = false;
	while (!saw_join) {
		auto status = pending->ExecuteTask();
		if (PendingQueryResult::IsExecutionFinished(status)) {
			break;
		}
		// operator ids are pre-order positions in the profiled tree: RESULT_COLLECTOR -> UNGROUPED_AGGREGATE ->
		// HASH_JOIN; the per-tag breakdown sums to the operator's current usage
		auto result = inspector.Query(
		    "SELECT operator_id, memory_usage_bytes <= peak_memory_usage_bytes, connection_id = " + connection_id +
		    ", coalesce(list_sum(map_values(memory_usage_bytes_by_tag)), 0) = memory_usage_bytes "
		    "FROM duckdb_operator_memory() WHERE operator_name = 'HASH_JOIN' AND peak_memory_usage_bytes > 0");
		REQUIRE_NO_FAIL(*result);
		if (result->RowCount() == 0) {
			continue;
		}
		REQUIRE(CHECK_COLUMN(result, 0, {2}));
		REQUIRE(CHECK_COLUMN(result, 1, {true}));
		REQUIRE(CHECK_COLUMN(result, 2, {true}));
		REQUIRE(CHECK_COLUMN(result, 3, {true}));
		saw_join = true;
	}
	REQUIRE(saw_join);
	auto result = pending->Execute();
	REQUIRE(CHECK_COLUMN(result, 0, {1000000}));

	// once the query has finished none of its operators are listed
	result = inspector.Query("SELECT count(*) FROM duckdb_operator_memory()");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
}
