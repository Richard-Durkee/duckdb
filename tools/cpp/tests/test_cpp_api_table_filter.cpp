#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: filters pushed into the scan of a table function.
//
// cpp_blocks(n) produces n blocks of BLOCK_ROWS rows: "block" (the block number), "v" (block * BLOCK_ROWS + the row's
// position in the block) and "s" (NULL for every fifth row, else the row's v as text). Before reading a block, the scan
// checks every pushed filter against the block's range - as a file reader would against the statistics of a row group
// - and skips the block when a filter rules it out. It never applies a filter to the rows it produces: the engine does.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

constexpr int64_t BLOCK_ROWS = 100;

//! How a registered variant of cpp_blocks behaves
struct BlocksMode {
	//! Whether the scan skips the blocks a filter rules out
	bool skip_blocks = true;
	//! Whether the scan claims its rows already satisfy the filters, without applying them
	bool claim_filters_applied = false;
};

//! What the last scan observed, read by the tests after the query
struct BlocksObservations {
	std::atomic<idx_t> blocks_read {0};
	std::atomic<idx_t> blocks_skipped {0};
	idx_t filter_count = 0;
	std::vector<idx_t> filter_columns;
	std::vector<bool> filter_optional;
	std::vector<ExpressionType> filter_types;
	//! For each filter, the declared column its expression references, or -1
	std::vector<int64_t> expression_columns;

	void Reset() {
		blocks_read = 0;
		blocks_skipped = 0;
		filter_count = 0;
		filter_columns.clear();
		filter_optional.clear();
		filter_types.clear();
		expression_columns.clear();
	}
};

BlocksObservations observed;

struct BlocksBind {
	int64_t block_count = 0;
};

struct BlocksGlobal {
	std::vector<TableFilter> filters;
	int64_t next_block = 0;
	//! The block being produced, and the next row of it
	int64_t block = -1;
	int64_t row = 0;
};

int64_t FindColumnRef(const Expression &expression) {
	if (expression.GetType() == ExpressionType::BOUND_COLUMN_REF) {
		return static_cast<int64_t>(expression.GetColumnIndex());
	}
	for (idx_t i = 0; i < expression.GetChildCount(); i++) {
		auto found = FindColumnRef(expression.GetChild(i));
		if (found >= 0) {
			return found;
		}
	}
	return -1;
}

void BlocksBindCb(TableFunction::BindInput &input) {
	const auto count = input.GetConstantArgument(0).Get<int64_t>();
	auto &factory = input.GetContext().GetFactory();
	input.AddResultColumn("block", factory.ParseType("BIGINT"));
	input.AddResultColumn("v", factory.ParseType("BIGINT"));
	input.AddResultColumn("s", factory.ParseType("VARCHAR"));
	input.SetCardinality(static_cast<idx_t>(count * BLOCK_ROWS), true);
	input.SetBindData<BlocksBind>(BlocksBind {count});
}

void BlocksInitGlobal(TableFunction::InitGlobalInput &input) {
	BlocksGlobal global;
	observed.filter_count = input.GetFilterCount();
	for (idx_t i = 0; i < input.GetFilterCount(); i++) {
		auto filter = input.GetFilter(i);
		observed.filter_columns.push_back(filter.GetColumnIndex());
		observed.filter_optional.push_back(filter.IsOptional());
		auto expression = filter.GetExpression();
		observed.filter_types.push_back(expression.GetType());
		observed.expression_columns.push_back(FindColumnRef(expression));
		global.filters.push_back(std::move(filter));
	}
	input.SetGlobalState<BlocksGlobal>(std::move(global));
}

//! Whether some filter rules out every row of the block, judging by the range of its columns
bool BlockRuledOut(Factory &factory, const BlocksGlobal &global, int64_t block) {
	for (auto &filter : global.filters) {
		auto column = filter.GetColumnIndex();
		FilterPropagateResult result;
		if (column == 0) {
			auto value = factory.CreateValue(block);
			result = filter.CheckRange(&value, &value, false, true);
		} else if (column == 1) {
			auto min = factory.CreateValue(block * BLOCK_ROWS);
			auto max = factory.CreateValue(block * BLOCK_ROWS + BLOCK_ROWS - 1);
			result = filter.CheckRange(&min, &max, false, true);
		} else {
			// nothing is known about the strings but that some are NULL
			result = filter.CheckRange(nullptr, nullptr, true, true);
		}
		if (result == FilterPropagateResult::FILTER_ALWAYS_FALSE ||
		    result == FilterPropagateResult::FILTER_FALSE_OR_NULL) {
			return true;
		}
	}
	return false;
}

void BlocksExec(TableFunction::ExecInput &input) {
	const auto &bind = input.GetBindData<BlocksBind>();
	const auto &mode = input.GetUserData<BlocksMode>();
	auto &global = input.GetGlobalState<BlocksGlobal>();
	auto &factory = input.GetContext().GetFactory();

	auto chunk = input.GetOutputChunk();
	// start the next block that the filters do not rule out
	while (global.block < 0 || global.row == BLOCK_ROWS) {
		if (global.next_block >= bind.block_count) {
			chunk.GetVector(0).SetSize(0);
			return;
		}
		auto block = global.next_block++;
		if (mode.skip_blocks && BlockRuledOut(factory, global, block)) {
			observed.blocks_skipped++;
			continue;
		}
		observed.blocks_read++;
		global.block = block;
		global.row = 0;
	}

	auto count = std::min<int64_t>(BLOCK_ROWS - global.row, static_cast<int64_t>(chunk.GetCapacity()));
	for (idx_t c = 0; c < input.GetColumnCount(); c++) {
		auto vector = chunk.GetVector(c);
		vector.SetSize(static_cast<idx_t>(count));
		auto column = input.GetColumnIndex(c);
		for (int64_t i = 0; i < count; i++) {
			auto v = global.block * BLOCK_ROWS + global.row + i;
			if (column == 0) {
				vector.GetDataMutable<int64_t>()[i] = global.block;
			} else if (column == 1) {
				vector.GetDataMutable<int64_t>()[i] = v;
			} else if (v % 5 == 0) {
				vector.SetNull(static_cast<idx_t>(i));
			} else {
				vector.AssignString(static_cast<idx_t>(i), std::to_string(v));
			}
		}
	}
	global.row += count;
	if (mode.claim_filters_applied) {
		input.SetFiltersApplied();
	}
}

void RegisterBlocks(Connection &conn, const std::string &name, BlocksMode mode,
                    TableFunction::FilterPushdownCallback expression_callback = nullptr) {
	auto &factory = conn.GetFactory();
	auto function = TableFunction::Create(factory);
	function.SetName(name);
	function.GetSignature().AddParameter("n", factory.ParseType("BIGINT"));
	function.SetBindCallback(BlocksBindCb)
	    .SetInitGlobalCallback(BlocksInitGlobal)
	    .SetExecCallback(BlocksExec)
	    .SetProjectionPushdown(true)
	    .SetFilterPushdown(true)
	    .SetUserData<BlocksMode>(mode);
	if (expression_callback) {
		function.SetFilterPushdownCallback(expression_callback);
	}
	conn.Register(function);
}

int64_t QueryBigint(Connection &conn, const std::string &sql) {
	auto result = conn.Execute(sql);
	auto chunk = result.FetchChunk();
	REQUIRE(chunk);
	auto view = chunk.GetVector(0).GetView();
	REQUIRE(view.IsValid(0));
	return view.Data<int64_t>()[view.SelAt(0)];
}

std::string QueryText(Connection &conn, const std::string &sql) {
	auto result = conn.Execute(sql);
	std::string text;
	while (auto chunk = result.FetchChunk()) {
		for (idx_t row = 0; row < chunk.GetRowCount(); row++) {
			for (idx_t col = 0; col < chunk.GetVectorCount(); col++) {
				text += chunk.GetVector(col).GetValue(row).ToText() + (col + 1 < chunk.GetVectorCount() ? "," : "\n");
			}
		}
	}
	return text;
}

//! The expression filter pushdown callback of the variant that also takes expressions: records what it was offered,
//! accepting nothing
idx_t expressions_offered = 0;
void RecordExpressions(TableFunction::FilterPushdownInput &input) {
	expressions_offered += input.GetFilterCount();
}

} // namespace

TEST_CASE("Stable C++API: pushed filters give the same results as filters above the scan", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterBlocks(conn, "cpp_blocks", BlocksMode {});
	RegisterBlocks(conn, "cpp_blocks_noskip", BlocksMode {false, false});
	conn.Execute("CREATE TABLE expected AS SELECT * FROM cpp_blocks_noskip(20)").Drain();
	REQUIRE(QueryBigint(conn, "SELECT count(*) FROM expected") == 2000);

	const std::vector<std::string> predicates {
	    "v >= 1500",
	    "v < 250",
	    "v BETWEEN 300 AND 410",
	    "v = 1999",
	    "v <> 5",
	    "s IS NULL",
	    "s IS NOT NULL AND v < 50",
	    "v IN (5, 999, 1500, 7777)",
	    "v = 7 OR v = 1999",
	    "block = 3 AND v % 2 = 0",
	    "s = '1234'",
	    "s > '9' AND block >= 10",
	    "v > 100000",
	};
	for (auto &predicate : predicates) {
		INFO(predicate);
		for (auto &function : {"cpp_blocks(20)", "cpp_blocks_noskip(20)"}) {
			auto actual = QueryText(conn, std::string("SELECT count(*), sum(v), count(s), min(s), max(block) FROM ") +
			                                  function + " WHERE " + predicate);
			auto expected = QueryText(
			    conn, "SELECT count(*), sum(v), count(s), min(s), max(block) FROM expected WHERE " + predicate);
			REQUIRE(actual == expected);
		}
	}
	// the rows themselves, not only aggregates
	REQUIRE(QueryText(conn, "SELECT v, s FROM cpp_blocks(20) WHERE v BETWEEN 1495 AND 1505 ORDER BY v") ==
	        QueryText(conn, "SELECT v, s FROM expected WHERE v BETWEEN 1495 AND 1505 ORDER BY v"));
}

TEST_CASE("Stable C++API: a scan skips the blocks a pushed filter rules out", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterBlocks(conn, "cpp_blocks", BlocksMode {});

	observed.Reset();
	REQUIRE(QueryBigint(conn, "SELECT count(*) FROM cpp_blocks(20) WHERE v >= 1500") == 500);
	REQUIRE(observed.blocks_read == 5);
	REQUIRE(observed.blocks_skipped == 15);

	observed.Reset();
	REQUIRE(QueryBigint(conn, "SELECT count(*) FROM cpp_blocks(20) WHERE v BETWEEN 300 AND 410") == 111);
	REQUIRE(observed.blocks_read == 2);

	observed.Reset();
	REQUIRE(QueryBigint(conn, "SELECT count(*) FROM cpp_blocks(20) WHERE block = 7 AND v % 2 = 0") == 50);
	REQUIRE(observed.blocks_read == 1);

	// a block whose strings are not known to the scan is read, whatever the filter on them
	observed.Reset();
	REQUIRE(QueryBigint(conn, "SELECT count(*) FROM cpp_blocks(20) WHERE s = '1203'") == 1);
	REQUIRE(observed.blocks_read == 20);

	// filters the range can not rule out keep every block
	observed.Reset();
	REQUIRE(QueryBigint(conn, "SELECT count(*) FROM cpp_blocks(20) WHERE v % 7 = 0") == 286);
	REQUIRE(observed.blocks_read == 20);
}

TEST_CASE("Stable C++API: a scan sees the filters pushed into it", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterBlocks(conn, "cpp_blocks", BlocksMode {});

	observed.Reset();
	REQUIRE(QueryBigint(conn, "SELECT count(*) FROM cpp_blocks(20) WHERE v >= 1500") == 500);
	REQUIRE(observed.filter_count == 1);
	REQUIRE(observed.filter_columns == std::vector<idx_t> {1});
	REQUIRE(observed.filter_optional == std::vector<bool> {false});
	REQUIRE(observed.filter_types == std::vector<ExpressionType> {ExpressionType::COMPARE_GREATERTHANOREQUALTO});
	// the column appears in the predicate as a reference to the declared column
	REQUIRE(observed.expression_columns == std::vector<int64_t> {1});

	observed.Reset();
	REQUIRE(QueryBigint(conn, "SELECT count(*) FROM cpp_blocks(20) WHERE s = '203' AND block = 2") == 1);
	REQUIRE(observed.filter_count == 2);
	auto columns = observed.filter_columns;
	std::sort(columns.begin(), columns.end());
	REQUIRE(columns == std::vector<idx_t> {0, 2});
	REQUIRE(observed.blocks_read == 1);

	// no filter, no filters
	observed.Reset();
	REQUIRE(QueryBigint(conn, "SELECT count(*) FROM cpp_blocks(20)") == 2000);
	REQUIRE(observed.filter_count == 0);
	REQUIRE(observed.blocks_read == 20);
}

TEST_CASE("Stable C++API: filters derived while the query runs reach the scan", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterBlocks(conn, "cpp_blocks", BlocksMode {});
	conn.Execute("CREATE TABLE keys AS SELECT 1700 + range AS k FROM range(3)").Drain();

	// a join: the build side's range of keys becomes a filter on the probe side's scan
	observed.Reset();
	REQUIRE(QueryBigint(conn, "SELECT count(*) FROM cpp_blocks(20) b JOIN keys ON b.v = keys.k") == 3);
	REQUIRE(observed.filter_count >= 1);
	REQUIRE(std::find(observed.filter_optional.begin(), observed.filter_optional.end(), true) !=
	        observed.filter_optional.end());
	REQUIRE(observed.blocks_read == 1);

	// a top-N: once it holds enough rows, the rows it would not keep are filtered out of the scan
	observed.Reset();
	REQUIRE(QueryText(conn, "SELECT v FROM cpp_blocks(20) ORDER BY v LIMIT 3") == "0\n1\n2\n");
	REQUIRE(observed.blocks_read < 20);
}

TEST_CASE("Stable C++API: the engine applies pushed filters unless the scan says it did", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterBlocks(conn, "cpp_blocks_noskip", BlocksMode {false, false});
	RegisterBlocks(conn, "cpp_blocks_claimed", BlocksMode {false, true});

	// every batch but one is filtered down to no rows - which must not end the scan
	REQUIRE(QueryBigint(conn, "SELECT count(*) FROM cpp_blocks_noskip(20) WHERE v = 1999") == 1);
	REQUIRE(QueryBigint(conn, "SELECT count(*) FROM cpp_blocks_noskip(20) WHERE v <> 5") == 1999);
	// a scan that claims to have applied the filters is taken at its word
	REQUIRE(QueryBigint(conn, "SELECT count(*) FROM cpp_blocks_claimed(20) WHERE v <> 5") == 2000);
}

TEST_CASE("Stable C++API: filter pushdown alongside the expression filter pushdown callback", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterBlocks(conn, "cpp_blocks_expr", BlocksMode {}, RecordExpressions);

	// the expression callback is offered the predicates first; those it leaves become table filters
	expressions_offered = 0;
	observed.Reset();
	REQUIRE(QueryBigint(conn, "SELECT count(*) FROM cpp_blocks_expr(20) WHERE v >= 1500") == 500);
	REQUIRE(expressions_offered >= 1);
	REQUIRE(observed.filter_count == 1);
	REQUIRE(observed.blocks_read == 5);
}

TEST_CASE("Stable C++API: filter pushdown requires projection pushdown", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	auto function = TableFunction::Create(factory);
	function.SetName("cpp_blocks_noproj");
	function.GetSignature().AddParameter("n", factory.ParseType("BIGINT"));
	function.SetBindCallback(BlocksBindCb)
	    .SetInitGlobalCallback(BlocksInitGlobal)
	    .SetExecCallback(BlocksExec)
	    .SetFilterPushdown(true)
	    .SetUserData<BlocksMode>(BlocksMode {});
	REQUIRE_THROWS_AS(conn.Register(function), Exception);
}
