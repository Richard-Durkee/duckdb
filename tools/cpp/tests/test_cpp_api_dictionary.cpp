#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: Dictionary and Vector::MakeDictionary, through a table function that produces the same values
// either as DICTIONARY vectors over one dictionary or as FLAT vectors with a copy of a value per row.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

//! The distinct values the scan produces; row i holds VALUES[i % VALUE_COUNT], or NULL when i % NULL_EVERY == 0
const std::vector<std::string> VALUES = {"apple",      "banana", "cherry", "a value long enough to live in the heap",
                                         "elderberry", "fig",    "grape"};
constexpr int64_t NULL_EVERY = 11;

struct ScanBind {
	int64_t count = 0;
	bool use_dictionary = false;
};

struct ScanGlobal {
	explicit ScanGlobal(const LogicalType &type) : dictionary(type, VALUES.size() + 1) {
		// the last entry is the NULL the NULL rows select
		auto entries = dictionary.GetVector();
		for (idx_t i = 0; i < VALUES.size(); i++) {
			entries.AssignString(i, VALUES[i]);
		}
		entries.SetNull(VALUES.size());
	}

	Dictionary dictionary;
	int64_t position = 0;
	//! How many DICTIONARY vectors the scan produced
	idx_t dictionary_chunks = 0;
};

ScanGlobal *last_scan = nullptr;

void ScanBind_(TableFunction::BindInput &input) {
	ScanBind bind;
	bind.count = input.GetConstantArgument(0).Get<int64_t>();
	bind.use_dictionary = input.GetConstantArgument(1).Get<bool>();
	input.AddResultColumn("s", input.GetContext().GetFactory().ParseType("VARCHAR"));
	input.AddResultColumn("i", input.GetContext().GetFactory().ParseType("BIGINT"));
	input.SetBindData<ScanBind>(bind);
}

void ScanInitGlobal(TableFunction::InitGlobalInput &input) {
	auto varchar = input.GetContext().GetFactory().ParseType("VARCHAR");
	input.SetGlobalState<ScanGlobal>(varchar);
}

uint32_t EntryOf(int64_t row) {
	return row % NULL_EVERY == 0 ? static_cast<uint32_t>(VALUES.size()) : static_cast<uint32_t>(row % VALUES.size());
}

void ScanExec(TableFunction::ExecInput &input) {
	const auto &bind = input.GetBindData<ScanBind>();
	auto &global = input.GetGlobalState<ScanGlobal>();
	last_scan = &global;
	auto chunk = input.GetOutputChunk();

	auto rows = std::min<int64_t>(bind.count - global.position, static_cast<int64_t>(chunk.GetCapacity()));
	auto strings = chunk.GetVector(0);
	auto numbers = chunk.GetVector(1);
	numbers.SetSize(static_cast<idx_t>(rows));
	auto *number_data = numbers.GetDataMutable<int64_t>();
	for (int64_t i = 0; i < rows; i++) {
		number_data[i] = global.position + i;
	}
	if (bind.use_dictionary) {
		std::vector<uint32_t> sel(static_cast<size_t>(rows));
		for (int64_t i = 0; i < rows; i++) {
			sel[i] = EntryOf(global.position + i);
		}
		strings.MakeDictionary(global.dictionary, sel);
		if (rows > 0) {
			REQUIRE(strings.GetVectorType() == VectorType::DICTIONARY);
			global.dictionary_chunks++;
		}
	} else {
		strings.SetSize(static_cast<idx_t>(rows));
		for (int64_t i = 0; i < rows; i++) {
			auto entry = EntryOf(global.position + i);
			if (entry == VALUES.size()) {
				strings.SetNull(static_cast<idx_t>(i));
			} else {
				strings.AssignString(static_cast<idx_t>(i), VALUES[entry]);
			}
		}
	}
	global.position += rows;
}

void RegisterScan(Connection &conn) {
	auto &factory = conn.GetFactory();
	auto function = TableFunction::Create(factory);
	function.SetName("cpp_dictionary_scan");
	function.GetSignature().AddParameter("n", factory.ParseType("BIGINT"));
	function.GetSignature().AddParameter("use_dictionary", factory.ParseType("BOOLEAN"));
	function.SetBindCallback(ScanBind_).SetInitGlobalCallback(ScanInitGlobal).SetExecCallback(ScanExec);
	conn.Register(function);
}

//! The rows of a result, each rendered as one string
std::vector<std::string> CollectRows(QueryResult result) {
	std::vector<std::string> rows;
	while (auto chunk = result.FetchChunk()) {
		for (idx_t row = 0; row < chunk.GetRowCount(); row++) {
			std::string text;
			for (idx_t col = 0; col < chunk.GetVectorCount(); col++) {
				auto value = chunk.GetVector(col).GetValue(row);
				text += (col ? "|" : "") + (value.IsNull() ? std::string("NULL") : value.ToText());
			}
			rows.push_back(std::move(text));
		}
	}
	return rows;
}

} // namespace

TEST_CASE("Stable C++API: a table function produces DICTIONARY vectors", "[cpp_api][dictionary]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterScan(conn);

	// the same rows as the FLAT scan, row for row, NULLs included
	const char *query = "SELECT s, i FROM cpp_dictionary_scan(10000, %s) ORDER BY i";
	char dict_query[100], flat_query[100];
	snprintf(dict_query, sizeof(dict_query), query, "true");
	snprintf(flat_query, sizeof(flat_query), query, "false");
	auto dict_rows = CollectRows(conn.Execute(dict_query));
	// every chunk selected from the one dictionary
	REQUIRE(last_scan != nullptr);
	REQUIRE(last_scan->dictionary_chunks > 1);

	REQUIRE(dict_rows.size() == 10000);
	REQUIRE(dict_rows == CollectRows(conn.Execute(flat_query)));
	REQUIRE(dict_rows[0] == "NULL|0");
	REQUIRE(dict_rows[3] == "a value long enough to live in the heap|3");
}

TEST_CASE("Stable C++API: operators over DICTIONARY vectors match the FLAT results", "[cpp_api][dictionary]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterScan(conn);

	// grouping, hashing, joining and functions all have paths specialized for dictionaries
	const std::vector<std::string> queries = {
	    "SELECT s, count(*), sum(i) FROM cpp_dictionary_scan(50000, %s) GROUP BY s ORDER BY s",
	    "SELECT count(DISTINCT s), sum(hash(s)) FROM cpp_dictionary_scan(50000, %s)",
	    "SELECT upper(s), length(s), count(*) FROM cpp_dictionary_scan(50000, %s) GROUP BY ALL ORDER BY ALL",
	    "SELECT count(*) FROM cpp_dictionary_scan(20000, %s) a JOIN (SELECT DISTINCT s FROM cpp_dictionary_scan(100, "
	    "false)) b USING (s)",
	    "SELECT s, i FROM cpp_dictionary_scan(5000, %s) WHERE s LIKE '%%e%%' ORDER BY i LIMIT 20"};
	for (auto &query : queries) {
		char dict_query[300], flat_query[300];
		snprintf(dict_query, sizeof(dict_query), query.c_str(), "true");
		snprintf(flat_query, sizeof(flat_query), query.c_str(), "false");
		INFO(query);
		auto dict_rows = CollectRows(conn.Execute(dict_query));
		REQUIRE(!dict_rows.empty());
		REQUIRE(dict_rows == CollectRows(conn.Execute(flat_query)));
	}
}

TEST_CASE("Stable C++API: Dictionary and MakeDictionary", "[cpp_api][dictionary]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	auto integer = factory.ParseType("INTEGER");

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("INTEGER"));
	DataChunk chunk(factory, types);
	auto vec = chunk.GetVector(0);
	{
		Dictionary dictionary(integer, 3);
		REQUIRE(dictionary.GetSize() == 3);
		auto *entries = dictionary.GetVector().GetDataMutable<int32_t>();
		entries[0] = 7;
		entries[1] = 8;
		entries[2] = 9;
		vec.MakeDictionary(dictionary, std::vector<uint32_t> {2, 2, 0, 1});
		REQUIRE(vec.GetVectorType() == VectorType::DICTIONARY);

		// errors surface as exceptions and leave the vector alone
		REQUIRE_THROWS_AS(vec.MakeDictionary(dictionary, std::vector<uint32_t> {3}), InvalidInputException);
		Dictionary other(factory.ParseType("VARCHAR"), 1);
		REQUIRE_THROWS_AS(vec.MakeDictionary(other, std::vector<uint32_t> {0}), InvalidInputException);
	}
	// the vector keeps the entries alive once the dictionary is gone
	auto view = vec.GetView();
	REQUIRE(view.count == 4);
	std::vector<int32_t> values;
	for (idx_t i = 0; i < view.count; i++) {
		values.push_back(view.Data<int32_t>()[view.SelAt(i)]);
	}
	REQUIRE(values == std::vector<int32_t> {9, 9, 7, 8});

	REQUIRE_THROWS_AS(Dictionary(factory.ParseType("STRUCT(a INTEGER)"), 1), InvalidInputException);
}
