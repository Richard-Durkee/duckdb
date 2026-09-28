#include "catch.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
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

} // namespace

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
