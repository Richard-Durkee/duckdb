#include "catch.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

struct StepRecord {
	OptimizerType type;
	OptimizerStepPhase phase;
	bool has_join_estimate;
};

struct StepRecorderInfo : public OptimizerExtensionInfo {
	vector<StepRecord> records;
};

bool HasJoinWithEstimate(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN && op.has_estimated_cardinality) {
		return true;
	}
	for (auto &child : op.children) {
		if (HasJoinWithEstimate(*child)) {
			return true;
		}
	}
	return false;
}

void RecordStep(OptimizerStepInput &input) {
	auto &info = static_cast<StepRecorderInfo &>(*input.info);
	info.records.push_back({input.type, input.phase, HasJoinWithEstimate(input.plan)});
}

idx_t CountSteps(const vector<StepRecord> &records, OptimizerType type, OptimizerStepPhase phase) {
	idx_t count = 0;
	for (auto &record : records) {
		count += record.type == type && record.phase == phase;
	}
	return count;
}

} // namespace

TEST_CASE("Test optimizer step callback", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t1 AS SELECT range AS i FROM range(1000)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t2 AS SELECT range AS i FROM range(100)"));

	auto info = make_shared_ptr<StepRecorderInfo>();
	OptimizerExtension extension;
	extension.optimizer_step_function = RecordStep;
	extension.optimizer_info = info;
	OptimizerExtension::Register(DBConfig::GetConfig(*db.instance), std::move(extension));

	REQUIRE_NO_FAIL(con.Query("SELECT count(*) FROM t1 JOIN t2 USING (i) WHERE t1.i > 10"));
	auto &records = info->records;
	REQUIRE(!records.empty());
	for (auto type : {OptimizerType::FILTER_PUSHDOWN, OptimizerType::JOIN_ORDER}) {
		auto before = CountSteps(records, type, OptimizerStepPhase::BEFORE);
		REQUIRE(before >= 1);
		REQUIRE(CountSteps(records, type, OptimizerStepPhase::AFTER) == before);
	}
	// every BEFORE is directly followed by the AFTER of the same step
	REQUIRE(records.size() % 2 == 0);
	for (idx_t i = 0; i < records.size(); i += 2) {
		REQUIRE(records[i].phase == OptimizerStepPhase::BEFORE);
		REQUIRE(records[i + 1].phase == OptimizerStepPhase::AFTER);
		REQUIRE(records[i].type == records[i + 1].type);
	}
	// the join order optimizer assigns estimated cardinalities to the joins
	for (idx_t i = 0; i < records.size(); i++) {
		if (records[i].type == OptimizerType::JOIN_ORDER) {
			REQUIRE(!records[i].has_join_estimate);
			REQUIRE(records[i + 1].has_join_estimate);
			break;
		}
	}

	// disabled optimizers are not reported
	records.clear();
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='filter_pushdown'"));
	REQUIRE_NO_FAIL(con.Query("SELECT count(*) FROM t1 JOIN t2 USING (i) WHERE t1.i > 10"));
	REQUIRE(CountSteps(records, OptimizerType::FILTER_PUSHDOWN, OptimizerStepPhase::BEFORE) == 0);
	REQUIRE(CountSteps(records, OptimizerType::JOIN_ORDER, OptimizerStepPhase::AFTER) == 1);
}
