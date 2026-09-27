#include "catch.hpp"

#include "duckdb/main/database.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/temporary_memory_manager.hpp"
#include "test_helpers.hpp"

using namespace duckdb; // NOLINT

TEST_CASE("TemporaryMemoryManager handles many active states", "[storage][temporary_memory_manager]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;
	auto &buffer_pool = DatabaseInstance::GetDatabase(context).GetBufferPool();

	constexpr idx_t memory_limit = 1024ULL * 1024ULL * 1024ULL;
	constexpr idx_t state_count = 80;
	constexpr idx_t initial_reservation = 10ULL * 1024ULL * 1024ULL;
	constexpr idx_t remaining_size = 1024ULL * 1024ULL * 1024ULL;

	buffer_pool.SetLimit(memory_limit, "temporary memory manager test");

	auto &manager = TemporaryMemoryManager::Get(context);
	duckdb::vector<duckdb::unique_ptr<TemporaryMemoryState>> states;
	states.reserve(state_count);
	for (idx_t i = 0; i < state_count; i++) {
		auto state = manager.Register(context, "TEST");
		state->SetMinimumReservation(initial_reservation);
		state->SetRemainingSize(remaining_size);
		states.push_back(std::move(state));
	}

	REQUIRE_NO_FAIL(con.Query("SET debug_force_external=true"));
	for (auto &state : states) {
		state->UpdateReservation(context);
		REQUIRE(state->GetReservation() == initial_reservation);
	}

	REQUIRE_NO_FAIL(con.Query("SET debug_force_external=false"));
	states.back()->UpdateReservation(context);

	REQUIRE(states.back()->GetReservation() >= initial_reservation);
	REQUIRE(states.back()->GetReservation() <= remaining_size);
}

TEST_CASE("duckdb_temporary_memory reports active states", "[storage][temporary_memory_manager]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection inspector(db);
	auto &context = *con.context;

	constexpr idx_t minimum_reservation = 10ULL * 1024ULL * 1024ULL;
	auto state = TemporaryMemoryManager::Get(context).Register(context, "TEST");
	state->SetMinimumReservation(minimum_reservation);
	state->SetRemainingSize(minimum_reservation);
	state->UpdateReservation(context);

	auto result = inspector.Query("SELECT operator_name, connection_id, query_id IS NULL, reservation_bytes, "
	                              "remaining_size_bytes, minimum_reservation_bytes FROM duckdb_temporary_memory()");
	REQUIRE(CHECK_COLUMN(result, 0, {"TEST"}));
	REQUIRE(CHECK_COLUMN(result, 1, {Value::UBIGINT(context.GetConnectionId())}));
	REQUIRE(CHECK_COLUMN(result, 2, {true}));
	REQUIRE(CHECK_COLUMN(result, 3, {Value::BIGINT(NumericCast<int64_t>(state->GetReservation()))}));
	REQUIRE(CHECK_COLUMN(result, 4, {Value::BIGINT(minimum_reservation)}));
	REQUIRE(CHECK_COLUMN(result, 5, {Value::BIGINT(minimum_reservation)}));

	state.reset();
	result = inspector.Query("SELECT count(*) FROM duckdb_temporary_memory()");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
}

TEST_CASE("duckdb_temporary_memory reports the states of a running query", "[storage][temporary_memory_manager]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection inspector(db);

	// pause a streaming hash join mid-probe: its sink state, and with it its temporary memory state, is alive
	auto streaming = con.SendQuery("SELECT a.range FROM range(1000000) a JOIN range(1000000) b ON a.range = b.range");
	REQUIRE_NO_FAIL(*streaming);
	auto chunk = streaming->Fetch();
	REQUIRE(chunk);

	auto result =
	    inspector.Query("SELECT count(*), bool_and(connection_id = " + to_string(con.context->GetConnectionId()) +
	                    "), bool_and(query_id IS NOT NULL) FROM duckdb_temporary_memory() "
	                    "WHERE operator_name = 'HASH_JOIN'");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(CHECK_COLUMN(result, 1, {true}));
	REQUIRE(CHECK_COLUMN(result, 2, {true}));

	// an abandoned streaming result keeps its query alive until the connection runs its next query
	streaming.reset();
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
	result = inspector.Query("SELECT count(*) FROM duckdb_temporary_memory()");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
}
