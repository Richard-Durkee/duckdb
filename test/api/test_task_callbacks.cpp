#include "catch.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

struct CountingTaskState : public ClientContextState {
	atomic<idx_t> starts {0};
	atomic<idx_t> stops {0};

	void OnTaskStart(ClientContext &context) override {
		starts++;
	}
	void OnTaskStop(ClientContext &context) override {
		stops++;
	}
};

struct OptOutTaskState : public CountingTaskState {
	bool ReceivesTaskCallbacks() const override {
		return false;
	}
};

struct TaskRecord {
	string task_type;
	bool has_pipeline;
	bool has_operator;
	PhysicalOperatorType operator_type;
	bool is_stop;
	TaskExecutionResult result;
};

struct RecordingTaskState : public ClientContextState {
	mutex lock;
	vector<TaskRecord> records;

	void OnTaskStart(ClientContext &context, const TaskInfo &info) override {
		Record(info, false);
	}
	void OnTaskStop(ClientContext &context, const TaskInfo &info) override {
		Record(info, true);
	}

	void Record(const TaskInfo &info, bool is_stop) {
		TaskRecord record;
		record.task_type = info.task.TaskType();
		record.has_pipeline = info.pipeline != nullptr;
		record.has_operator = info.op != nullptr;
		record.operator_type = info.op ? info.op->type : PhysicalOperatorType::INVALID;
		record.is_stop = is_stop;
		record.result = info.result;
		lock_guard<mutex> guard(lock);
		records.push_back(record);
	}

	vector<TaskRecord> Take() {
		lock_guard<mutex> guard(lock);
		auto result = std::move(records);
		records.clear();
		return result;
	}
};

} // namespace

TEST_CASE("Test task callbacks receive the task identity", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=4"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE build AS SELECT range AS i FROM range(2000000)"));
	auto state = con.context->registered_state->GetOrCreate<RecordingTaskState>("recording_state");

	// a parallel scan runs as pipeline tasks that know their pipeline
	REQUIRE_NO_FAIL(con.Query("SELECT sum(i) FROM range(10000000) t(i)"));
	auto records = state->Take();
	idx_t pipeline_task_starts = 0;
	idx_t starts = 0;
	idx_t stops = 0;
	for (auto &record : records) {
		if (record.is_stop) {
			stops++;
			REQUIRE(record.result != TaskExecutionResult::TASK_ERROR);
		} else {
			starts++;
		}
		if (record.task_type == "PipelineTask") {
			REQUIRE(record.has_pipeline);
			pipeline_task_starts += !record.is_stop;
		}
	}
	REQUIRE(starts == stops);
	REQUIRE(pipeline_task_starts > 1);

	// the hash join finalize tasks know both their pipeline and their operator
	REQUIRE_NO_FAIL(con.Query("SELECT count(*) FROM build b1 JOIN build b2 USING (i)"));
	records = state->Take();
	idx_t finalize_stops = 0;
	for (auto &record : records) {
		if (record.task_type != "HashJoinFinalizeTask") {
			continue;
		}
		REQUIRE(record.has_pipeline);
		REQUIRE(record.has_operator);
		REQUIRE(record.operator_type == PhysicalOperatorType::HASH_JOIN);
		if (record.is_stop) {
			REQUIRE(record.result == TaskExecutionResult::TASK_FINISHED);
			finalize_stops++;
		}
	}
	REQUIRE(finalize_stops > 0);

	// a failing task reports TASK_ERROR on stop
	REQUIRE_FAIL(con.Query("SELECT CASE WHEN i = 5000000 THEN error('boom') END FROM range(10000000) t(i)"));
	records = state->Take();
	bool saw_error = false;
	for (auto &record : records) {
		saw_error = saw_error || (record.is_stop && record.result == TaskExecutionResult::TASK_ERROR);
	}
	REQUIRE(saw_error);
}

TEST_CASE("Test task callbacks only reach states that receive them", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=4"));
	auto &states = *con.context->registered_state;
	REQUIRE(!states.HasTaskListeners());

	auto opt_out = make_shared_ptr<OptOutTaskState>();
	states.Insert("opt_out", opt_out);
	REQUIRE(!states.HasTaskListeners());

	auto listener = states.GetOrCreate<CountingTaskState>("listener");
	REQUIRE(states.HasTaskListeners());

	REQUIRE_NO_FAIL(con.Query("SELECT sum(i) FROM range(10000000) t(i)"));
	REQUIRE(listener->starts > 0);
	REQUIRE(listener->starts == listener->stops);
	REQUIRE(opt_out->starts == 0);
	REQUIRE(opt_out->stops == 0);

	// removing the only listener disables task callbacks again
	states.Remove("listener");
	REQUIRE(!states.HasTaskListeners());
	auto starts_before = listener->starts.load();
	REQUIRE_NO_FAIL(con.Query("SELECT sum(i) FROM range(10000000) t(i)"));
	REQUIRE(listener->starts == starts_before);

	// a re-inserted listener receives callbacks again
	states.Insert("listener", listener);
	REQUIRE(states.HasTaskListeners());
	REQUIRE_NO_FAIL(con.Query("SELECT sum(i) FROM range(10000000) t(i)"));
	REQUIRE(listener->starts > starts_before);
	REQUIRE(listener->starts == listener->stops);
}
