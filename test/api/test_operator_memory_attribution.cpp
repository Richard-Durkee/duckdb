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
