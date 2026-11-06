#include "test_capi_v2.hpp"

#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// V2 memory tests: blocks of memory the database accounts for, under the
// EXTENSION tag of duckdb_memory(), and against memory_limit.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

constexpr idx_t MEMORY_TEST_MIB = 1024 * 1024;

int64_t MemoryTestQueryI64(duckdb_v2_connection_handle conn, std::string_view sql) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(result);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	auto out = static_cast<const int64_t *>(view.data)[SelAt(view.sel, 0)];
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&result);
	return out;
}

// The memory the database accounts to the EXTENSION tag.
int64_t MemoryTestExtensionUsage(duckdb_v2_connection_handle conn) {
	return MemoryTestQueryI64(conn, "SELECT memory_usage_bytes::BIGINT FROM duckdb_memory() WHERE tag = 'EXTENSION'");
}

uint8_t *MemoryTestData(duckdb_v2_memory_buffer_handle buffer) {
	uint8_t *data = nullptr;
	REQUIRE(duckdb_v2_memory_buffer_get_data(buffer, &data, nullptr) == DUCKDB_V2_ERROR_NONE);
	return data;
}

// Whether the first `count` bytes hold the pattern the resize test writes.
bool MemoryTestHasPattern(const uint8_t *data, idx_t count) {
	for (idx_t i = 0; i < count; i++) {
		if (data[i] != static_cast<uint8_t>(i % 251)) {
			return false;
		}
	}
	return true;
}

idx_t MemoryTestSize(duckdb_v2_memory_buffer_handle buffer) {
	idx_t size = 0;
	REQUIRE(duckdb_v2_memory_buffer_get_size(buffer, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	return size;
}

} // namespace

TEST_CASE("V2 memory: an allocation is accounted until it is destroyed", "[capi_v2][memory]") {
	EnvFixture fx;
	auto base = MemoryTestExtensionUsage(fx.conn);

	duckdb_v2_memory_buffer_handle buffer = nullptr;
	REQUIRE(duckdb_v2_instance_allocate_memory(fx.instance, 8 * MEMORY_TEST_MIB, &buffer, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(buffer != nullptr);
	REQUIRE(MemoryTestSize(buffer) == 8 * MEMORY_TEST_MIB);
	auto data = MemoryTestData(buffer);
	REQUIRE(data != nullptr);
	// the whole block is writable
	std::memset(data, 0xAB, 8 * MEMORY_TEST_MIB);
	REQUIRE(data[8 * MEMORY_TEST_MIB - 1] == 0xAB);
	REQUIRE(MemoryTestExtensionUsage(fx.conn) == base + static_cast<int64_t>(8 * MEMORY_TEST_MIB));

	REQUIRE(duckdb_v2_memory_buffer_destroy(&buffer) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(buffer == nullptr);
	REQUIRE(MemoryTestExtensionUsage(fx.conn) == base);

	// null-safe
	REQUIRE(duckdb_v2_memory_buffer_destroy(&buffer) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_memory_buffer_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 memory: resizing keeps the contents and follows the accounting", "[capi_v2][memory]") {
	EnvFixture fx;
	auto base = MemoryTestExtensionUsage(fx.conn);

	duckdb_v2_memory_buffer_handle buffer = nullptr;
	REQUIRE(duckdb_v2_instance_allocate_memory(fx.instance, MEMORY_TEST_MIB, &buffer, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto data = MemoryTestData(buffer);
	for (idx_t i = 0; i < MEMORY_TEST_MIB; i++) {
		data[i] = static_cast<uint8_t>(i % 251);
	}

	REQUIRE(duckdb_v2_memory_buffer_resize(buffer, 4 * MEMORY_TEST_MIB, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(MemoryTestSize(buffer) == 4 * MEMORY_TEST_MIB);
	REQUIRE(MemoryTestExtensionUsage(fx.conn) == base + static_cast<int64_t>(4 * MEMORY_TEST_MIB));
	data = MemoryTestData(buffer);
	REQUIRE(MemoryTestHasPattern(data, MEMORY_TEST_MIB));

	REQUIRE(duckdb_v2_memory_buffer_resize(buffer, 1000, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(MemoryTestSize(buffer) == 1000);
	REQUIRE(MemoryTestExtensionUsage(fx.conn) == base + 1000);
	data = MemoryTestData(buffer);
	REQUIRE(MemoryTestHasPattern(data, 1000));

	// an empty buffer counts nothing, and can grow again
	REQUIRE(duckdb_v2_memory_buffer_resize(buffer, 0, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(MemoryTestSize(buffer) == 0);
	REQUIRE(MemoryTestData(buffer) == nullptr);
	REQUIRE(MemoryTestExtensionUsage(fx.conn) == base);
	REQUIRE(duckdb_v2_memory_buffer_resize(buffer, 100, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(MemoryTestData(buffer) != nullptr);
	REQUIRE(MemoryTestExtensionUsage(fx.conn) == base + 100);

	duckdb_v2_memory_buffer_destroy(&buffer);
	REQUIRE(MemoryTestExtensionUsage(fx.conn) == base);
}

TEST_CASE("V2 memory: an allocation over the memory limit fails with out of memory", "[capi_v2][memory]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "SET memory_limit = '64MB'");
	auto base = MemoryTestExtensionUsage(fx.conn);

	duckdb_v2_memory_buffer_handle buffer = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_instance_allocate_memory(fx.instance, 256 * MEMORY_TEST_MIB, &buffer, &err) ==
	        DUCKDB_V2_ERROR_RESOURCE_OUT_OF_MEMORY);
	REQUIRE(buffer == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);
	// nothing was accounted
	REQUIRE(MemoryTestExtensionUsage(fx.conn) == base);

	// a block that fits can not grow past the limit, and is left as it was
	REQUIRE(duckdb_v2_instance_allocate_memory(fx.instance, 16 * MEMORY_TEST_MIB, &buffer, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	auto data = MemoryTestData(buffer);
	data[0] = 42;
	REQUIRE(duckdb_v2_memory_buffer_resize(buffer, 256 * MEMORY_TEST_MIB, nullptr) ==
	        DUCKDB_V2_ERROR_RESOURCE_OUT_OF_MEMORY);
	REQUIRE(MemoryTestSize(buffer) == 16 * MEMORY_TEST_MIB);
	REQUIRE(MemoryTestData(buffer) == data);
	REQUIRE(data[0] == 42);
	REQUIRE(MemoryTestExtensionUsage(fx.conn) == base + static_cast<int64_t>(16 * MEMORY_TEST_MIB));

	duckdb_v2_memory_buffer_destroy(&buffer);
	REQUIRE(MemoryTestExtensionUsage(fx.conn) == base);
}

TEST_CASE("V2 memory: an empty allocation and invalid arguments", "[capi_v2][memory]") {
	EnvFixture fx;
	auto base = MemoryTestExtensionUsage(fx.conn);

	duckdb_v2_memory_buffer_handle buffer = nullptr;
	REQUIRE(duckdb_v2_instance_allocate_memory(fx.instance, 0, &buffer, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(buffer != nullptr);
	REQUIRE(MemoryTestSize(buffer) == 0);
	REQUIRE(MemoryTestData(buffer) == nullptr);
	REQUIRE(MemoryTestExtensionUsage(fx.conn) == base);
	duckdb_v2_memory_buffer_destroy(&buffer);

	// a size beyond what can be allocated is rejected before anything is accounted
	REQUIRE(duckdb_v2_instance_allocate_memory(fx.instance, ~idx_t(0), &buffer, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_OUT_OF_RANGE);
	REQUIRE(buffer == nullptr);
	REQUIRE(MemoryTestExtensionUsage(fx.conn) == base);

	REQUIRE(duckdb_v2_instance_allocate_memory(nullptr, 10, &buffer, nullptr) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_instance_allocate_memory(fx.instance, 10, nullptr, nullptr) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_context_allocate_memory(nullptr, 10, &buffer, nullptr) != DUCKDB_V2_ERROR_NONE);
	uint8_t *data = nullptr;
	idx_t size = 0;
	REQUIRE(duckdb_v2_memory_buffer_get_data(nullptr, &data, nullptr) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_memory_buffer_get_size(nullptr, &size, nullptr) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_memory_buffer_resize(nullptr, 10, nullptr) != DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 memory: a buffer may outlive its database", "[capi_v2][memory]") {
	duckdb_v2_environment_handle env = nullptr;
	duckdb_v2_instance_handle instance = nullptr;
	REQUIRE(duckdb_v2_environment_create(&env, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(OpenInstance(env, std::string_view(), &instance, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_memory_buffer_handle buffer = nullptr;
	REQUIRE(duckdb_v2_instance_allocate_memory(instance, MEMORY_TEST_MIB, &buffer, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_instance_destroy(&instance);

	// the bytes are still there, but the buffer can no longer grow: there is no database to account for it
	auto data = MemoryTestData(buffer);
	data[MEMORY_TEST_MIB - 1] = 7;
	REQUIRE(duckdb_v2_memory_buffer_resize(buffer, 2 * MEMORY_TEST_MIB, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(MemoryTestSize(buffer) == MEMORY_TEST_MIB);
	REQUIRE(duckdb_v2_memory_buffer_destroy(&buffer) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_environment_destroy(&env);
}

} // namespace test_capi_v2
