#include "catch.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/profiler/metrics.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

//! Records the query text that the connection publishes to other connections while each query runs
class RecordQueryActivity : public ClientContextState {
public:
	void QueryBegin(ClientContext &context) override {
		auto activity = context.GetQueryActivity();
		REQUIRE(activity.running);
		published_queries.push_back(activity.query);
	}

	vector<string> published_queries;
};

shared_ptr<RecordQueryActivity> RecordActivity(Connection &con) {
	auto state = make_shared_ptr<RecordQueryActivity>();
	con.context->registered_state->Insert("record_query_activity", state);
	return state;
}

} // namespace

TEST_CASE("Test that the published query activity redacts secrets", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto state = RecordActivity(con);

	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
	REQUIRE_NO_FAIL(con.Query("CREATE SECRET s1 (TYPE http, BEARER_TOKEN 'TOPSECRET')"));
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TEMPORARY SECRET s2 (TYPE http, BEARER_TOKEN 'TOPSECRET')"));

	REQUIRE(state->published_queries.size() == 3);
	REQUIRE(state->published_queries[0] == "SELECT 42");
	REQUIRE(state->published_queries[1] == "CREATE SECRET <redacted>");
	REQUIRE(state->published_queries[2] == "CREATE SECRET <redacted>");

	// the activity is cleared once the query has finished
	REQUIRE(!con.context->GetQueryActivity().running);
	REQUIRE(con.context->GetQueryActivity().query.empty());
}

TEST_CASE("Test that the published query activity shows secrets when unredacted secrets are allowed", "[api]") {
	DBConfig config;
	config.SetOptionByName("allow_unredacted_secrets", true);
	DuckDB db(nullptr, &config);
	Connection con(db);
	auto state = RecordActivity(con);

	REQUIRE_NO_FAIL(con.Query("CREATE SECRET s1 (TYPE http, BEARER_TOKEN 'TOPSECRET')"));

	REQUIRE(state->published_queries.size() == 1);
	REQUIRE(StringUtil::Contains(state->published_queries[0], "TOPSECRET"));
}

TEST_CASE("Test that the live query metrics are labeled with the running query they belong to", "[api]") {
	auto path = TestCreatePath("query_activity_metrics.db");
	DeleteDatabase(path);
	{
		DuckDB db(path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range AS i FROM range(10000000)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}
	{
		DuckDB db(path);
		Connection con(db);

		// pause a streaming scan after its first chunk: its metrics are labeled with it
		auto streaming = OpenStream(con, "SELECT i FROM t");
		REQUIRE(streaming->Fetch());
		auto activity = con.context->GetQueryActivity();
		REQUIRE(activity.running);
		auto live = con.context->GetLiveQueryMetrics();
		REQUIRE(live.query_id.IsValid());
		REQUIRE(live.query_id.GetIndex() == activity.query_id);
		auto bytes_read = live.metrics.find(MetricIOTotalBytesRead::Name);
		REQUIRE(bytes_read != live.metrics.end());
		REQUIRE(bytes_read->second.GetValue<idx_t>() > 0);

		// once the scan has finished, the connection keeps its counters but no running query owns them
		while (streaming->Fetch()) {
		}
		REQUIRE(!con.context->GetQueryActivity().running);
		live = con.context->GetLiveQueryMetrics();
		REQUIRE(!live.query_id.IsValid());
		bytes_read = live.metrics.find(MetricIOTotalBytesRead::Name);
		REQUIRE(bytes_read != live.metrics.end());
		REQUIRE(bytes_read->second.GetValue<idx_t>() > 0);
	}
	DeleteDatabase(path);
}

TEST_CASE("Test that an open result the client is not reading is reported as waiting for the client", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection monitor(db);

	auto streaming = OpenStream(con, "SELECT range FROM range(10000000)");
	REQUIRE(streaming->Fetch());
	auto activity = con.context->GetQueryActivity();
	REQUIRE(activity.running);
	REQUIRE(activity.state == QueryActivityState::WAITING_FOR_CLIENT);

	auto result = monitor.Query("SELECT state FROM duckdb_queries() WHERE connection_id = " +
	                            to_string(con.context->GetConnectionId()));
	REQUIRE(CHECK_COLUMN(result, 0, {"waiting for client"}));

	// the monitoring connection is running its own query
	result = monitor.Query("SELECT state FROM duckdb_queries() WHERE connection_id = current_connection_id()");
	REQUIRE(CHECK_COLUMN(result, 0, {"running"}));

	streaming->Close();
	REQUIRE(con.context->GetQueryActivity().state == QueryActivityState::IDLE);
}
