#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: MemoryBuffer, memory the database accounts for.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

constexpr idx_t MEMORY_MIB = 1024 * 1024;

int64_t QueryMemoryI64(Connection &conn, const std::string &sql) {
	auto result = conn.Execute(sql);
	auto chunk = result.FetchChunk();
	REQUIRE(chunk);
	auto view = chunk.GetVector(0).GetView();
	REQUIRE(view.IsValid(0));
	return view.Data<int64_t>()[view.SelAt(0)];
}

// The memory the database accounts to the EXTENSION tag.
int64_t ExtensionMemoryUsage(Connection &conn) {
	return QueryMemoryI64(conn, "SELECT memory_usage_bytes::BIGINT FROM duckdb_memory() WHERE tag = 'EXTENSION'");
}

// ---------------------------------------------------------------------------
// cpp_tracked_scan(bytes): allocates `bytes` of accounted memory in its bind,
// keeps it in the bind data, and returns its size and the database's
// EXTENSION usage at the time of the scan.
// ---------------------------------------------------------------------------

struct TrackedBind {
	explicit TrackedBind(MemoryBuffer buffer_p) : buffer(std::move(buffer_p)) {
	}
	MemoryBuffer buffer;
};

struct TrackedGlobal {
	bool done = false;
};

void TrackedBindCallback(TableFunction::BindInput &input) {
	const auto bytes = input.GetConstantArgument(0).Get<int64_t>();
	auto &factory = input.GetContext().GetFactory();
	input.AddResultColumn("bytes", factory.ParseType("BIGINT"));
	auto buffer = input.GetContext().AllocateMemory(static_cast<idx_t>(bytes));
	if (bytes > 0) {
		std::memset(buffer.GetData(), 1, static_cast<size_t>(bytes));
	}
	input.SetBindData<TrackedBind>(std::move(buffer));
}

void TrackedInitGlobal(TableFunction::InitGlobalInput &input) {
	input.SetGlobalState<TrackedGlobal>();
}

void TrackedExec(TableFunction::ExecInput &input) {
	const auto &bind = input.GetBindData<TrackedBind>();
	auto &global = input.GetGlobalState<TrackedGlobal>();
	auto chunk = input.GetOutputChunk();
	auto vec = chunk.GetVector(0);
	if (global.done) {
		vec.SetSize(0);
		return;
	}
	vec.GetDataMutable<int64_t>()[0] = static_cast<int64_t>(bind.buffer.GetSize());
	vec.SetSize(1);
	global.done = true;
}

void RegisterTrackedScan(Connection &conn) {
	auto &factory = conn.GetFactory();
	auto function = TableFunction::Create(factory);
	function.SetName("cpp_tracked_scan");
	function.GetSignature().AddParameter("bytes", factory.ParseType("BIGINT"));
	function.SetBindCallback(TrackedBindCallback).SetInitGlobalCallback(TrackedInitGlobal).SetExecCallback(TrackedExec);
	conn.Register(function);
}

} // namespace

TEST_CASE("Stable C++API: memory buffer accounting", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	const auto base = ExtensionMemoryUsage(conn);

	{
		auto buffer = db.AllocateMemory(4 * MEMORY_MIB);
		REQUIRE(buffer.GetSize() == 4 * MEMORY_MIB);
		std::memset(buffer.GetData(), 0x5A, 4 * MEMORY_MIB);
		REQUIRE(buffer.GetData<uint32_t>()[0] == 0x5A5A5A5Au);
		REQUIRE(ExtensionMemoryUsage(conn) == base + static_cast<int64_t>(4 * MEMORY_MIB));

		// moving the buffer moves the ownership, not the accounting
		auto moved = std::move(buffer);
		REQUIRE(!buffer);
		REQUIRE(moved.GetSize() == 4 * MEMORY_MIB);
		REQUIRE(ExtensionMemoryUsage(conn) == base + static_cast<int64_t>(4 * MEMORY_MIB));

		moved.Resize(MEMORY_MIB);
		REQUIRE(moved.GetData<uint8_t>()[MEMORY_MIB - 1] == 0x5A);
		REQUIRE(ExtensionMemoryUsage(conn) == base + static_cast<int64_t>(MEMORY_MIB));
	}
	REQUIRE(ExtensionMemoryUsage(conn) == base);
}

TEST_CASE("Stable C++API: memory buffers respect the memory limit", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	conn.Execute("SET memory_limit = '64MB'").Drain();
	const auto base = ExtensionMemoryUsage(conn);

	REQUIRE_THROWS_MATCHES(db.AllocateMemory(256 * MEMORY_MIB), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_RESOURCE_OUT_OF_MEMORY));
	REQUIRE(ExtensionMemoryUsage(conn) == base);

	auto buffer = db.AllocateMemory(8 * MEMORY_MIB);
	REQUIRE_THROWS_MATCHES(buffer.Resize(256 * MEMORY_MIB), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_RESOURCE_OUT_OF_MEMORY));
	REQUIRE(buffer.GetSize() == 8 * MEMORY_MIB);
	REQUIRE(ExtensionMemoryUsage(conn) == base + static_cast<int64_t>(8 * MEMORY_MIB));
}

TEST_CASE("Stable C++API: a table function keeps accounted memory in its bind data", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterTrackedScan(conn);
	conn.Execute("SET memory_limit = '64MB'").Drain();
	const auto base = ExtensionMemoryUsage(conn);

	REQUIRE(QueryMemoryI64(conn, "SELECT * FROM cpp_tracked_scan(16 * 1024 * 1024)") ==
	        static_cast<int64_t>(16 * MEMORY_MIB));
	// the bind data, and with it the memory, is gone with the query
	REQUIRE(ExtensionMemoryUsage(conn) == base);

	// the allocation counts against the limit of the query's database
	REQUIRE_THROWS_MATCHES(conn.Execute("SELECT * FROM cpp_tracked_scan(256 * 1024 * 1024)").Drain(), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_RESOURCE_OUT_OF_MEMORY));
	REQUIRE(ExtensionMemoryUsage(conn) == base);
}

TEST_CASE("Stable C++API: a memory buffer may outlive its instance", "[cpp_api]") {
	std::vector<MemoryBuffer> buffers;
	{
		auto db = Instance(":memory:");
		buffers.push_back(db.AllocateMemory(MEMORY_MIB));
		buffers.push_back(db.AllocateMemory(0));
	}
	buffers[0].GetData()[0] = 1;
	REQUIRE_THROWS_MATCHES(buffers[0].Resize(2 * MEMORY_MIB), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE(buffers[1].GetData() == nullptr);
	buffers.clear();
}
