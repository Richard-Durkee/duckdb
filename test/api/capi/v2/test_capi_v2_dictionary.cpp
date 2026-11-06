#include "test_capi_v2.hpp"

// Internal engine headers: used only to read the dictionary id DuckDB recognizes a dictionary by, which no public
// surface exposes. Tests are the sanctioned mixing point.
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"

// ---------------------------------------------------------------------------
// V2 dictionary API tests: dictionary_create / dictionary_get_vector /
// dictionary_destroy, and vector_dictionary turning a chunk's vector into a
// DICTIONARY vector over the dictionary's entries.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

//! Creates a dictionary of `size` INTEGER entries holding entry * 10, and NULL at the indices in `nulls`
duckdb_v2_dictionary_handle MakeIntegerDictionary(duckdb_v2_factory_handle factory, idx_t size,
                                                  std::initializer_list<idx_t> nulls = {}) {
	auto int_type = MakeType(factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_dictionary_handle dictionary = nullptr;
	auto rc = duckdb_v2_dictionary_create(int_type, size, &dictionary, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(dictionary != nullptr);

	duckdb_v2_vector_handle entries = nullptr;
	REQUIRE(duckdb_v2_dictionary_get_vector(dictionary, &entries, nullptr) == DUCKDB_V2_ERROR_NONE);
	idx_t entry_count = 0;
	REQUIRE(duckdb_v2_vector_get_size(entries, &entry_count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(entry_count == size);
	void *data = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(entries, &data, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *values = static_cast<int32_t *>(data);
	for (idx_t i = 0; i < size; i++) {
		values[i] = static_cast<int32_t>(i * 10);
	}
	for (auto null_index : nulls) {
		REQUIRE(duckdb_v2_vector_set_null(entries, null_index, nullptr) == DUCKDB_V2_ERROR_NONE);
	}
	return dictionary;
}

//! Creates a chunk with a single INTEGER column
duckdb_v2_data_chunk_handle MakeIntegerChunk(duckdb_v2_factory_handle factory) {
	auto int_type = MakeType(factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(factory, &int_type, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	return chunk;
}

//! Reads an INTEGER vector through its view: the values, with NULL rows as -1
std::vector<int32_t> ReadIntegers(duckdb_v2_vector_handle vector) {
	duckdb_v2_vector_view view;
	REQUIRE(duckdb_v2_vector_get_view(vector, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	std::vector<int32_t> result;
	auto *data = static_cast<const int32_t *>(view.data);
	for (idx_t i = 0; i < view.count; i++) {
		idx_t idx = view.sel ? view.sel[i] : i;
		bool valid = !view.validity || (view.validity[idx >> 6] & (uint64_t(1) << (idx & 63)));
		result.push_back(valid ? data[idx] : -1);
	}
	return result;
}

} // namespace

TEST_CASE("V2: vector_dictionary selects the dictionary's entries", "[capi_v2][dictionary]") {
	EnvFixture fx;
	auto dictionary = MakeIntegerDictionary(fx.factory, 4, {3});
	auto chunk = MakeIntegerChunk(fx.factory);
	duckdb_v2_vector_handle vector = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vector, nullptr) == DUCKDB_V2_ERROR_NONE);

	const uint32_t sel[] = {2, 0, 0, 3, 1, 2};
	REQUIRE(duckdb_v2_vector_dictionary(vector, dictionary, sel, 6, nullptr) == DUCKDB_V2_ERROR_NONE);

	DUCKDB_V2_VECTOR_TYPE vector_type;
	REQUIRE(duckdb_v2_vector_get_vector_type(vector, &vector_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(vector_type == DUCKDB_V2_VECTOR_TYPE_DICTIONARY);
	idx_t size = 0;
	REQUIRE(duckdb_v2_vector_get_size(vector, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 6);

	// the view reads the dictionary's own entries through the selection: nothing was copied
	duckdb_v2_vector_view view;
	REQUIRE(duckdb_v2_vector_get_view(vector, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(view.sel != nullptr);
	REQUIRE(view.count == 6);
	duckdb_v2_vector_handle entries = nullptr;
	REQUIRE(duckdb_v2_dictionary_get_vector(dictionary, &entries, nullptr) == DUCKDB_V2_ERROR_NONE);
	void *entry_data = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(entries, &entry_data, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(view.data == entry_data);

	// a NULL entry makes the rows that select it NULL
	REQUIRE(ReadIntegers(vector) == std::vector<int32_t> {20, 0, 0, -1, 10, 20});

	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_dictionary_destroy(&dictionary);
}

TEST_CASE("V2: a dictionary vector outlives the dictionary handle and the selection array", "[capi_v2][dictionary]") {
	EnvFixture fx;
	auto dictionary = MakeIntegerDictionary(fx.factory, 3);
	auto chunk = MakeIntegerChunk(fx.factory);
	duckdb_v2_vector_handle vector = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vector, nullptr) == DUCKDB_V2_ERROR_NONE);

	std::vector<uint32_t> sel = {1, 2, 1};
	REQUIRE(duckdb_v2_vector_dictionary(vector, dictionary, sel.data(), sel.size(), nullptr) == DUCKDB_V2_ERROR_NONE);
	// the selection was copied, and the entries are shared with the vector
	sel.assign(sel.size(), 0);
	duckdb_v2_dictionary_destroy(&dictionary);
	REQUIRE(dictionary == nullptr);
	REQUIRE(ReadIntegers(vector) == std::vector<int32_t> {10, 20, 10});

	// a deep copy of the chunk holds the same values
	duckdb_v2_data_chunk_handle copy = nullptr;
	REQUIRE(duckdb_v2_data_chunk_copy(fx.factory, chunk, &copy, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_vector_handle copied = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(copy, 0, &copied, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ReadIntegers(copied) == std::vector<int32_t> {10, 20, 10});
	duckdb_v2_data_chunk_destroy(&copy);
}

TEST_CASE("V2: one dictionary backs the vectors of many chunks", "[capi_v2][dictionary]") {
	EnvFixture fx;
	auto dictionary = MakeIntegerDictionary(fx.factory, 5);
	auto chunk = MakeIntegerChunk(fx.factory);
	duckdb_v2_vector_handle vector = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vector, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto &internal = *reinterpret_cast<duckdb::Vector *>(vector);
	std::string dictionary_id;
	for (uint32_t round = 0; round < 10; round++) {
		// clearing makes the vector a writable FLAT vector again, ready for the next batch
		REQUIRE(duckdb_v2_data_chunk_clear(chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
		const uint32_t sel[] = {round % 5, (round + 1) % 5};
		REQUIRE(duckdb_v2_vector_dictionary(vector, dictionary, sel, 2, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(ReadIntegers(vector) == std::vector<int32_t> {static_cast<int32_t>(round % 5 * 10),
		                                                      static_cast<int32_t>((round + 1) % 5 * 10)});
		// every chunk carries the dictionary's id and size, by which DuckDB recognizes it as the same dictionary
		REQUIRE(duckdb::DictionaryVector::DictionarySize(internal).GetIndex() == 5);
		const auto &id = duckdb::DictionaryVector::DictionaryId(internal);
		REQUIRE(!id.empty());
		if (round == 0) {
			dictionary_id = id;
		}
		REQUIRE(id == dictionary_id);
	}
	// another dictionary has an id of its own
	auto other = MakeIntegerDictionary(fx.factory, 5);
	REQUIRE(duckdb_v2_data_chunk_clear(chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
	const uint32_t first[] = {0};
	REQUIRE(duckdb_v2_vector_dictionary(vector, other, first, 1, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb::DictionaryVector::DictionaryId(internal) != dictionary_id);
	duckdb_v2_dictionary_destroy(&other);
	REQUIRE(duckdb_v2_data_chunk_clear(chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
	DUCKDB_V2_VECTOR_TYPE vector_type;
	REQUIRE(duckdb_v2_vector_get_vector_type(vector, &vector_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(vector_type == DUCKDB_V2_VECTOR_TYPE_FLAT);
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_dictionary_destroy(&dictionary);
}

TEST_CASE("V2: vector_dictionary with no rows", "[capi_v2][dictionary]") {
	EnvFixture fx;
	auto dictionary = MakeIntegerDictionary(fx.factory, 2);
	auto chunk = MakeIntegerChunk(fx.factory);
	duckdb_v2_vector_handle vector = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vector, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_dictionary(vector, dictionary, nullptr, 0, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ReadIntegers(vector).empty());
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_dictionary_destroy(&dictionary);
}

TEST_CASE("V2: vector_dictionary rejects invalid input", "[capi_v2][dictionary]") {
	EnvFixture fx;
	auto dictionary = MakeIntegerDictionary(fx.factory, 3);
	auto chunk = MakeIntegerChunk(fx.factory);
	duckdb_v2_vector_handle vector = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vector, nullptr) == DUCKDB_V2_ERROR_NONE);

	SECTION("an index that is not below the dictionary's size") {
		const uint32_t sel[] = {0, 3};
		duckdb_v2_error_info_handle error = nullptr;
		REQUIRE(duckdb_v2_vector_dictionary(vector, dictionary, sel, 2, &error) == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(error != nullptr);
		duckdb_v2_error_info_destroy(&error);
		// the vector is left unchanged
		DUCKDB_V2_VECTOR_TYPE vector_type;
		REQUIRE(duckdb_v2_vector_get_vector_type(vector, &vector_type, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(vector_type == DUCKDB_V2_VECTOR_TYPE_FLAT);
	}
	SECTION("a dictionary of another type") {
		auto varchar_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
		duckdb_v2_dictionary_handle varchar_dictionary = nullptr;
		REQUIRE(duckdb_v2_dictionary_create(varchar_type, 1, &varchar_dictionary, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_logical_type_destroy(&varchar_type);
		const uint32_t sel[] = {0};
		REQUIRE(duckdb_v2_vector_dictionary(vector, varchar_dictionary, sel, 1, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_dictionary_destroy(&varchar_dictionary);
	}
	SECTION("the dictionary's own entries") {
		duckdb_v2_vector_handle entries = nullptr;
		REQUIRE(duckdb_v2_dictionary_get_vector(dictionary, &entries, nullptr) == DUCKDB_V2_ERROR_NONE);
		const uint32_t sel[] = {0};
		REQUIRE(duckdb_v2_vector_dictionary(entries, dictionary, sel, 1, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("missing arguments") {
		const uint32_t sel[] = {0};
		REQUIRE(duckdb_v2_vector_dictionary(nullptr, dictionary, sel, 1, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(duckdb_v2_vector_dictionary(vector, nullptr, sel, 1, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(duckdb_v2_vector_dictionary(vector, dictionary, nullptr, 1, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_dictionary_destroy(&dictionary);
}

TEST_CASE("V2: dictionary_create rejects STRUCT types and missing arguments", "[capi_v2][dictionary]") {
	EnvFixture fx;
	auto struct_type =
	    MakeStructType(fx.factory, {"a", "b"}, {DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR});
	duckdb_v2_dictionary_handle dictionary = nullptr;
	REQUIRE(duckdb_v2_dictionary_create(struct_type, 2, &dictionary, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(dictionary == nullptr);
	duckdb_v2_logical_type_destroy(&struct_type);

	REQUIRE(duckdb_v2_dictionary_create(nullptr, 2, &dictionary, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	REQUIRE(duckdb_v2_dictionary_create(int_type, 2, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_logical_type_destroy(&int_type);

	duckdb_v2_vector_handle entries = nullptr;
	REQUIRE(duckdb_v2_dictionary_get_vector(nullptr, &entries, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// destroying is null-safe
	REQUIRE(duckdb_v2_dictionary_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_dictionary_destroy(&dictionary) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2: a dictionary of VARCHAR entries", "[capi_v2][dictionary]") {
	EnvFixture fx;
	auto varchar_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	duckdb_v2_dictionary_handle dictionary = nullptr;
	REQUIRE(duckdb_v2_dictionary_create(varchar_type, 2, &dictionary, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_data_chunk_handle chunk = nullptr;
	REQUIRE(duckdb_v2_data_chunk_create(fx.factory, &varchar_type, 1, &chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&varchar_type);

	duckdb_v2_vector_handle entries = nullptr;
	REQUIRE(duckdb_v2_dictionary_get_vector(dictionary, &entries, nullptr) == DUCKDB_V2_ERROR_NONE);
	const std::string long_value = "a value long enough to live in the entries' heap";
	duckdb_v2_arena_handle heap = nullptr;
	REQUIRE(duckdb_v2_vector_get_arena(entries, &heap, nullptr) == DUCKDB_V2_ERROR_NONE);
	void *entry_data = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(entries, &entry_data, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *entry_slots = static_cast<duckdb_v2_bytes *>(entry_data);
	DUCKDB_V2_ERROR rc;
	entry_slots[0] = MakeString(heap, "short", 5, rc, nullptr);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	entry_slots[1] = MakeString(heap, long_value.data(), long_value.size(), rc, nullptr);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vector = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vector, nullptr) == DUCKDB_V2_ERROR_NONE);
	const uint32_t sel[] = {1, 0, 1};
	REQUIRE(duckdb_v2_vector_dictionary(vector, dictionary, sel, 3, nullptr) == DUCKDB_V2_ERROR_NONE);
	// the entries' heap is shared with the vector, so long values survive the dictionary handle
	duckdb_v2_dictionary_destroy(&dictionary);

	duckdb_v2_vector_view view;
	REQUIRE(duckdb_v2_vector_get_view(vector, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *slots = static_cast<const duckdb_v2_bytes *>(view.data);
	REQUIRE(Convert(slots[view.sel[0]]) == long_value);
	REQUIRE(Convert(slots[view.sel[1]]) == "short");
	REQUIRE(Convert(slots[view.sel[2]]) == long_value);
	duckdb_v2_data_chunk_destroy(&chunk);
}

} // namespace test_capi_v2
