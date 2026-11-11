#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/profiler/metrics_manager.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

using namespace duckdb;

// The metrics an extension reports through the QueryProfiler, from the functions it runs: these tests call the
// profiler the way an extension's scalar functions would.

static void RegisterFunction(Connection &con, ScalarFunction function) {
	CreateScalarFunctionInfo info(std::move(function));
	con.context->RunFunctionInTransaction(
	    [&]() { Catalog::GetSystemCatalog(*con.context).CreateFunction(*con.context, info); });
}

// A table of a million rows, profiling enabled and eight threads: a scan of the table runs on several threads, where a
// scan of range() would not.
static constexpr idx_t ROW_COUNT = 1000000;
static void SetUpParallelScan(Connection &con, const string &profile_name) {
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE numbers AS SELECT range AS i FROM range(" + to_string(ROW_COUNT) + ")"));
	REQUIRE_NO_FAIL(con.Query("SET threads = 8"));
	con.EnableProfiling();
	ClientConfig::GetConfig(*con.context).profiler_save_location = TestCreatePath(profile_name);
}

// The value of the metric `key` in the profile of the last query, or NULL when it is not there.
static Value GetProfiledMetric(Connection &con, const string &key) {
	Value result;
	QueryProfiler::Get(*con.context)
	    .GetFinalizedProfileUnderLock(
	        [&](optional_ptr<const GatheredMetrics> metrics, optional_ptr<const ProfilingNode>) {
		        REQUIRE(metrics);
		        auto entry = metrics->GetMetrics().find(key);
		        if (entry != metrics->GetMetrics().end()) {
			        result = entry->second;
		        }
	        });
	return result;
}

// Adds the rows of every chunk to a counter shared by all threads, and to one of 64 counters picked by the chunk's
// first value, so the threads also add new keys to the counters while others are adding to them.
static void CountRowsExec(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &profiler = QueryProfiler::Get(state.GetContext());
	auto first = args.data[0].GetValue(0).GetValue<int64_t>();
	profiler.AddToMetricCounter("test.rows_counted", args.size());
	profiler.AddToMetricCounter("test.rows_counted_by_chunk_" + to_string(first / STANDARD_VECTOR_SIZE % 64),
	                            args.size());
	result.Reference(args.data[0]);
}

TEST_CASE("Metric counters can be added to from several threads at once", "[api][intraquery]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// hundreds of thousands of chunks would take too long
		return;
	}
	DuckDB db(nullptr);
	Connection con(db);
	ScalarFunction fn("count_rows", {LogicalType::BIGINT}, LogicalType::BIGINT, CountRowsExec);
	fn.SetStability(FunctionStability::VOLATILE);
	RegisterFunction(con, std::move(fn));
	SetUpParallelScan(con, "test_metric_counters_output.txt");

	REQUIRE_NO_FAIL(con.Query("SELECT max(count_rows(i)) FROM numbers"));
	REQUIRE(GetProfiledMetric(con, "test.rows_counted") == Value::UBIGINT(ROW_COUNT));
	idx_t total_by_chunk = 0;
	for (idx_t chunk = 0; chunk < 64; chunk++) {
		auto value = GetProfiledMetric(con, "test.rows_counted_by_chunk_" + to_string(chunk));
		total_by_chunk += value.IsNull() ? 0 : value.GetValue<idx_t>();
	}
	REQUIRE(total_by_chunk == ROW_COUNT);
}

// Ends 16 scoped timers for every chunk, for 16 of 64 timing metrics picked by the chunk's first value, from every
// thread that runs the scan, so the timers also add new keys while others are ending. Core's own timers can end on
// several threads too, e.g. the vacuum tasks of a checkpoint.
static atomic<idx_t> timed_chunks {0};
static void TimeChunkExec(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &profiler = QueryProfiler::Get(state.GetContext());
	auto first = NumericCast<idx_t>(args.data[0].GetValue(0).GetValue<int64_t>()) / STANDARD_VECTOR_SIZE;
	for (idx_t i = 0; i < 16; i++) {
		auto timer = profiler.StartTimerInternal("test.chunk_time_" + to_string((first + i) % 64));
	}
	result.Reference(args.data[0]);
	timed_chunks++;
}

TEST_CASE("Scoped timers can end on several threads at once", "[api][intraquery]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// sixteen timers for each of the hundreds of thousands of chunks would take too long
		return;
	}
	DuckDB db(nullptr);
	Connection con(db);
	ScalarFunction fn("time_chunk", {LogicalType::BIGINT}, LogicalType::BIGINT, TimeChunkExec);
	fn.SetStability(FunctionStability::VOLATILE);
	RegisterFunction(con, std::move(fn));
	SetUpParallelScan(con, "test_metric_timers_output.txt");

	timed_chunks = 0;
	REQUIRE_NO_FAIL(con.Query("SELECT max(time_chunk(i)) FROM numbers"));
	REQUIRE(timed_chunks > 64);
	for (idx_t chunk = 0; chunk < 64; chunk++) {
		auto seconds = GetProfiledMetric(con, "test.chunk_time_" + to_string(chunk));
		REQUIRE(!seconds.IsNull());
		REQUIRE(seconds.GetValue<double>() > 0);
	}
}

// Adds one millisecond to a timing metric for every chunk, from every thread that runs the scan.
static atomic<idx_t> add_timing_calls {0};
static void AddTimingExec(DataChunk &args, ExpressionState &state, Vector &result) {
	QueryProfiler::Get(state.GetContext()).AddToMetricTiming("test.time_added", 1000000);
	add_timing_calls++;
	result.Reference(args.data[0]);
}

TEST_CASE("Time measured elsewhere can be added to a timing metric", "[api][intraquery]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// hundreds of thousands of chunks would take too long
		return;
	}
	DuckDB db(nullptr);
	Connection con(db);
	ScalarFunction fn("add_timing", {LogicalType::BIGINT}, LogicalType::BIGINT, AddTimingExec);
	fn.SetStability(FunctionStability::VOLATILE);
	RegisterFunction(con, std::move(fn));
	SetUpParallelScan(con, "test_metric_timing_output.txt");

	add_timing_calls = 0;
	REQUIRE_NO_FAIL(con.Query("SELECT max(add_timing(i)) FROM numbers"));
	// one millisecond for each call, reported in seconds
	REQUIRE(add_timing_calls > 1);
	auto seconds = GetProfiledMetric(con, "test.time_added");
	REQUIRE(!seconds.IsNull());
	REQUIRE(seconds.GetValue<double>() == Approx(static_cast<double>(add_timing_calls.load()) / 1000.0));
}

// While the query binds, before its plan is built: sets one metric if it is tracked, one whether or not it is, one that
// was not registered, and two of the profiler's own, one of which it may leave out.
static bool tracked_while_binding = false;
static unique_ptr<FunctionData> SetMetricBind(BindScalarFunctionInput &input) {
	auto &profiler = QueryProfiler::Get(input.GetClientContext());
	tracked_while_binding = profiler.MetricIsTracked("test.set_if_tracked");
	if (tracked_while_binding) {
		profiler.SetMetric("test.set_if_tracked", Value::UBIGINT(42));
	}
	profiler.SetMetric("test.set_always", Value::UBIGINT(7));
	profiler.SetMetric("test.not_registered", Value::UBIGINT(1));
	profiler.SetMetric("query.total_rows_scanned", Value::UBIGINT(123456789));
	profiler.SetMetric("query.total_bytes_scanned", Value::UBIGINT(123456789));
	return nullptr;
}

static void PassThroughExec(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(args.data[0]);
}

TEST_CASE("A metric can be set while the query binds", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	ScalarFunction fn("set_metric_while_binding", {LogicalType::BIGINT}, LogicalType::BIGINT, PassThroughExec);
	fn.SetBindCallback(SetMetricBind);
	RegisterFunction(con, std::move(fn));
	auto &metrics_manager = MetricsManager::Get(*con.context);
	metrics_manager.RegisterMetric({"test.set_if_tracked", "uint64", "Set while binding if tracked", "values"});
	metrics_manager.RegisterMetric({"test.set_always", "uint64", "Set while binding", "values"});
	// the profiler's own metrics cannot be registered
	metrics_manager.RegisterMetric({"query.total_bytes_scanned", "uint64", "Not an extension's", "bytes"});
	REQUIRE(!metrics_manager.IsRegisteredMetric("query.total_bytes_scanned"));
	metrics_manager.RegisterMetric({"optimizer.window_join_pushdown", "double", "Not an extension's", "seconds"});
	REQUIRE(!metrics_manager.IsRegisteredMetric("optimizer.window_join_pushdown"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE numbers AS SELECT range AS i FROM range(10)"));
	con.EnableProfiling();
	ClientConfig::GetConfig(*con.context).profiler_save_location = TestCreatePath("test_set_metric_output.txt");

	REQUIRE_NO_FAIL(con.Query("SELECT max(set_metric_while_binding(i)) FROM numbers"));
	REQUIRE(tracked_while_binding);
	REQUIRE(GetProfiledMetric(con, "test.set_if_tracked") == Value::UBIGINT(42));
	REQUIRE(GetProfiledMetric(con, "test.set_always") == Value::UBIGINT(7));
	REQUIRE(GetProfiledMetric(con, "test.not_registered").IsNull());
	// the profiler's own metrics keep its values, and stay out when it leaves them out
	auto rows_scanned = GetProfiledMetric(con, "query.total_rows_scanned");
	REQUIRE(!rows_scanned.IsNull());
	REQUIRE(rows_scanned.GetValue<idx_t>() == 10);
	// a scan of a table does not count the bytes it scans, so the profiler leaves this out
	REQUIRE(GetProfiledMetric(con, "query.total_bytes_scanned").IsNull());

	// neither is reported when it is not tracked
	REQUIRE_NO_FAIL(con.Query("SET tracked_metrics = ['query.*']"));
	REQUIRE_NO_FAIL(con.Query("SELECT max(set_metric_while_binding(i)) FROM numbers"));
	REQUIRE(!tracked_while_binding);
	REQUIRE(GetProfiledMetric(con, "test.set_if_tracked").IsNull());
	REQUIRE(GetProfiledMetric(con, "test.set_always").IsNull());
}

// The indexes of the metrics the next test registers, for the scalar function to update.
static MetricIndex rows_index;
static MetricIndex time_index;
static MetricIndex count_value_index;
static MetricIndex ratio_value_index;
static atomic<idx_t> indexed_calls {0};

// Updates registered metrics by index for every chunk, from every thread that runs the scan.
static void UpdateByIndexExec(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &profiler = QueryProfiler::Get(state.GetContext());
	profiler.AddToMetric(rows_index, args.size());
	profiler.AddToMetric(time_index, 1000000);
	profiler.SetMetric(count_value_index, Value::UBIGINT(7));
	profiler.SetMetric(ratio_value_index, Value::DOUBLE(2.5));
	indexed_calls++;
	result.Reference(args.data[0]);
}

TEST_CASE("Registered metrics can be updated by index from several threads at once", "[api][intraquery]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// hundreds of thousands of chunks would take too long
		return;
	}
	DuckDB db(nullptr);
	Connection con(db);
	auto &metrics_manager = MetricsManager::Get(*con.context);
	rows_index = metrics_manager.RegisterMetric(
	    {"test.indexed_rows", "uint64", "Rows counted by index", "rows", MetricKind::COUNTER});
	time_index = metrics_manager.RegisterMetric(
	    {"test.indexed_time", "double", "Time added by index", "seconds", MetricKind::TIMING});
	count_value_index =
	    metrics_manager.RegisterMetric({"test.indexed_count", "uint64", "A count set by index", "values"});
	ratio_value_index =
	    metrics_manager.RegisterMetric({"test.indexed_ratio", "double", "A ratio set by index", "ratio"});
	ScalarFunction fn("update_by_index", {LogicalType::BIGINT}, LogicalType::BIGINT, UpdateByIndexExec);
	fn.SetStability(FunctionStability::VOLATILE);
	RegisterFunction(con, std::move(fn));
	SetUpParallelScan(con, "test_indexed_metrics_output.txt");

	indexed_calls = 0;
	REQUIRE_NO_FAIL(con.Query("SELECT max(update_by_index(i)) FROM numbers"));
	REQUIRE(indexed_calls > 1);
	REQUIRE(GetProfiledMetric(con, "test.indexed_rows") == Value::UBIGINT(ROW_COUNT));
	auto seconds = GetProfiledMetric(con, "test.indexed_time");
	REQUIRE(!seconds.IsNull());
	REQUIRE(seconds.GetValue<double>() == Approx(static_cast<double>(indexed_calls.load()) / 1000.0));
	REQUIRE(GetProfiledMetric(con, "test.indexed_count") == Value::UBIGINT(7));
	REQUIRE(GetProfiledMetric(con, "test.indexed_ratio") == Value::DOUBLE(2.5));

	// a query that does not update them does not report them
	REQUIRE_NO_FAIL(con.Query("SELECT max(i) FROM numbers"));
	REQUIRE(GetProfiledMetric(con, "test.indexed_rows").IsNull());
}

TEST_CASE("Registering a metric returns its index", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &metrics_manager = MetricsManager::Get(*con.context);
	auto first = metrics_manager.RegisterMetric({"test.first", "uint64", "First", "rows", MetricKind::COUNTER});
	auto second = metrics_manager.RegisterMetric({"test.second", "uint64", "Second", "rows", MetricKind::COUNTER});
	REQUIRE(first.IsValid());
	REQUIRE(second.IsValid());
	REQUIRE(first.GetIndex() != second.GetIndex());
	// registering a name again returns the index it already has
	auto again = metrics_manager.RegisterMetric({"test.first", "uint64", "First", "rows", MetricKind::COUNTER});
	REQUIRE(again.GetIndex() == first.GetIndex());
	// the profiler's own metrics get no index, and updating through it does nothing
	auto own = metrics_manager.RegisterMetric({"query.cpu_time", "double", "Not an extension's", "seconds"});
	REQUIRE(!own.IsValid());
	auto &profiler = QueryProfiler::Get(*con.context);
	profiler.AddToMetric(own, 1);
	profiler.SetMetric(own, Value::DOUBLE(1));
}

TEST_CASE("The profiler's own timings are kept by index, with their start and end", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE numbers AS SELECT range AS i FROM range(1000)"));
	con.EnableProfiling();
	ClientConfig::GetConfig(*con.context).profiler_save_location = TestCreatePath("test_core_indexed_output.txt");
	REQUIRE_NO_FAIL(con.Query("SELECT max(i) FROM numbers WHERE i % 7 = 0"));

	// reported as before
	auto planner_time = GetProfiledMetric(con, "planner.total_time");
	REQUIRE(!planner_time.IsNull());
	REQUIRE(planner_time.GetValue<double>() > 0);
	REQUIRE(!GetProfiledMetric(con, "query.total_time").IsNull());
	REQUIRE(!GetProfiledMetric(con, "optimizer.filter_pushdown").IsNull());

	// and read back by name, with the bounds the metrics log uses
	auto &query_metrics = QueryProfiler::Get(*con.context).GetQueryMetrics();
	REQUIRE(query_metrics.GetStringMetricInSeconds("planner.total_time") == Approx(planner_time.GetValue<double>()));
	auto timings = query_metrics.GetMetricTimings();
	REQUIRE(timings.count("planner.total_time") == 1);
	REQUIRE(timings["planner.total_time"].has_bounds);
	REQUIRE(timings["planner.total_time"].start_time <= timings["planner.total_time"].end_time);
}

// Registers a metric the first time it runs, while the query is running, and counts by it.
static atomic<bool> registered_while_running {false};
static MetricIndex late_index;
static void RegisterWhileRunningExec(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetClientContext();
	if (!registered_while_running.exchange(true)) {
		late_index = MetricsManager::Get(context).RegisterMetric(
		    {"test.registered_while_running", "uint64", "Registered while a query ran", "rows", MetricKind::COUNTER});
	}
	QueryProfiler::Get(context).AddToMetric(late_index, args.size());
	result.Reference(args.data[0]);
}

TEST_CASE("A metric registered while a query runs is reported from the next query on", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	ScalarFunction fn("register_while_running", {LogicalType::BIGINT}, LogicalType::BIGINT, RegisterWhileRunningExec);
	fn.SetStability(FunctionStability::VOLATILE);
	RegisterFunction(con, std::move(fn));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE numbers AS SELECT range AS i FROM range(100)"));
	REQUIRE_NO_FAIL(con.Query("SET threads = 1"));
	con.EnableProfiling();
	ClientConfig::GetConfig(*con.context).profiler_save_location = TestCreatePath("test_late_registration_output.txt");

	registered_while_running = false;
	REQUIRE_NO_FAIL(con.Query("SELECT max(register_while_running(i)) FROM numbers"));
	REQUIRE(GetProfiledMetric(con, "test.registered_while_running").IsNull());

	REQUIRE_NO_FAIL(con.Query("SELECT max(register_while_running(i)) FROM numbers"));
	REQUIRE(GetProfiledMetric(con, "test.registered_while_running") == Value::UBIGINT(100));
}

// Times every chunk with a scoped timer for a registered timing, by its index, from every thread that runs the scan.
static MetricIndex chunk_timer_index;
static void TimeByIndexExec(DataChunk &args, ExpressionState &state, Vector &result) {
	auto timer = QueryProfiler::Get(state.GetContext()).StartTimer(chunk_timer_index);
	result.Reference(args.data[0]);
}

TEST_CASE("Scoped timers of registered timings can end on several threads at once", "[api][intraquery]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// hundreds of thousands of chunks would take too long
		return;
	}
	DuckDB db(nullptr);
	Connection con(db);
	chunk_timer_index = MetricsManager::Get(*con.context)
	                        .RegisterMetric({"test.chunk_time_by_index", "double", "Time spent per chunk", "seconds",
	                                         MetricKind::TIMING});
	ScalarFunction fn("time_by_index", {LogicalType::BIGINT}, LogicalType::BIGINT, TimeByIndexExec);
	fn.SetStability(FunctionStability::VOLATILE);
	RegisterFunction(con, std::move(fn));
	SetUpParallelScan(con, "test_index_timers_output.txt");

	REQUIRE_NO_FAIL(con.Query("SELECT max(time_by_index(i)) FROM numbers"));
	auto seconds = GetProfiledMetric(con, "test.chunk_time_by_index");
	REQUIRE(!seconds.IsNull());
	REQUIRE(seconds.GetValue<double>() > 0);
}

// Runs a callback for every chunk, as an extension's scalar function would, so a test can update metrics in a query.
static std::function<void(QueryProfiler &, idx_t)> chunk_callback;
static void ChunkCallbackExec(DataChunk &args, ExpressionState &state, Vector &result) {
	chunk_callback(QueryProfiler::Get(state.GetContext()), args.size());
	result.Reference(args.data[0]);
}

static void SetUpChunkCallback(Connection &con, const string &profile_name) {
	ScalarFunction fn("chunk_callback", {LogicalType::BIGINT}, LogicalType::BIGINT, ChunkCallbackExec);
	fn.SetStability(FunctionStability::VOLATILE);
	RegisterFunction(con, std::move(fn));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE numbers AS SELECT range AS i FROM range(10000)"));
	REQUIRE_NO_FAIL(con.Query("SET threads = 1"));
	con.EnableProfiling();
	ClientConfig::GetConfig(*con.context).profiler_save_location = TestCreatePath(profile_name);
}

static constexpr const char *CHUNK_CALLBACK_QUERY = "SELECT max(chunk_callback(i)) FROM numbers";

TEST_CASE("A registered metric updated by name and by index adds up in one place", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &metrics_manager = MetricsManager::Get(*con.context);
	auto count_index = metrics_manager.RegisterMetric(
	    {"test.mixed_count", "uint64", "Counted both ways", "rows", MetricKind::COUNTER});
	auto time_index =
	    metrics_manager.RegisterMetric({"test.mixed_time", "double", "Timed both ways", "seconds", MetricKind::TIMING});
	auto value_index = metrics_manager.RegisterMetric({"test.mixed_value", "uint64", "Set both ways", "values"});
	SetUpChunkCallback(con, "test_mixed_updates_output.txt");

	atomic<bool> updated {false};
	chunk_callback = [&](QueryProfiler &profiler, idx_t) {
		if (updated.exchange(true)) {
			return;
		}
		for (idx_t i = 0; i < 5; i++) {
			profiler.AddToMetric(count_index, 1);
		}
		for (idx_t i = 0; i < 3; i++) {
			profiler.AddToMetricCounter("test.mixed_count", 1);
		}
		profiler.AddToMetric(time_index, 1000000000);
		profiler.AddToMetricTiming("test.mixed_time", 2000000000);
		profiler.SetMetric(value_index, Value::UBIGINT(1));
		profiler.SetMetric("test.mixed_value", Value::UBIGINT(2));
	};
	REQUIRE_NO_FAIL(con.Query(CHUNK_CALLBACK_QUERY));
	REQUIRE(GetProfiledMetric(con, "test.mixed_count") == Value::UBIGINT(8));
	REQUIRE(GetProfiledMetric(con, "test.mixed_time").GetValue<double>() == Approx(3.0));
	REQUIRE(GetProfiledMetric(con, "test.mixed_value") == Value::UBIGINT(2));
	// the timings and counters the HTML renderer and the metrics log read agree with the profile
	auto &query_metrics = QueryProfiler::Get(*con.context).GetQueryMetrics();
	REQUIRE(query_metrics.GetMetricCounters()["test.mixed_count"] == 8);
	REQUIRE(static_cast<double>(query_metrics.GetMetricTimings()["test.mixed_time"].duration) / 1e9 == Approx(3.0));
	// a timer started by name times through the index
	updated = false;
	chunk_callback = [&](QueryProfiler &profiler, idx_t) {
		if (!updated.exchange(true)) {
			auto timer = profiler.StartTimerInternal("test.mixed_time");
		}
	};
	REQUIRE_NO_FAIL(con.Query(CHUNK_CALLBACK_QUERY));
	REQUIRE(!GetProfiledMetric(con, "test.mixed_time").IsNull());
	REQUIRE(query_metrics.GetMetricTimings()["test.mixed_time"].has_bounds);
}

TEST_CASE("A metric's kind must fit its type", "[api]") {
	DuckDB db(nullptr);
	auto &metrics_manager = MetricsManager::Get(*db.instance);
	REQUIRE_THROWS_WITH(
	    metrics_manager.RegisterMetric({"test.bad_counter", "double", "Not a uint64", "rows", MetricKind::COUNTER}),
	    Catch::Contains("cannot have type"));
	REQUIRE_THROWS_WITH(
	    metrics_manager.RegisterMetric({"test.bad_timing", "uint64", "Not a double", "seconds", MetricKind::TIMING}),
	    Catch::Contains("cannot have type"));
	REQUIRE_THROWS_WITH(metrics_manager.RegisterMetric({"test.bad_type", "integer", "No such type", "rows"}),
	                    Catch::Contains("expected"));
	// a name registered again must keep its type and kind
	metrics_manager.RegisterMetric({"test.kept", "uint64", "Kept", "rows", MetricKind::COUNTER});
	REQUIRE_THROWS_WITH(metrics_manager.RegisterMetric({"test.kept", "uint64", "Kept", "rows", MetricKind::VALUE}),
	                    Catch::Contains("already registered"));
	REQUIRE_THROWS_WITH(metrics_manager.RegisterMetric({"test.kept", "double", "Kept", "rows", MetricKind::VALUE}),
	                    Catch::Contains("already registered"));
	REQUIRE(!metrics_manager.IsRegisteredMetric("test.bad_counter"));
}

TEST_CASE("Updates that do not fit a metric are left out", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &metrics_manager = MetricsManager::Get(*con.context);
	auto counter =
	    metrics_manager.RegisterMetric({"test.fit_counter", "uint64", "A counter", "rows", MetricKind::COUNTER});
	auto count = metrics_manager.RegisterMetric({"test.fit_count", "uint64", "A count", "rows"});
	auto ratio = metrics_manager.RegisterMetric({"test.fit_ratio", "double", "A ratio", "ratio"});
	auto label = metrics_manager.RegisterMetric({"test.fit_label", "string", "A label", ""});
	SetUpChunkCallback(con, "test_fitting_updates_output.txt");

	chunk_callback = [&](QueryProfiler &profiler, idx_t) {
		// a counter is added to, not set
		profiler.SetMetric(counter, Value::UBIGINT(100));
		profiler.AddToMetric(counter, 1);
		// a value is set, not added to, and only when it casts to the metric's type
		profiler.AddToMetric(count, 100);
		profiler.SetMetric(count, Value::BIGINT(3));
		profiler.SetMetric(count, Value::BIGINT(-1));
		profiler.SetMetric(count, Value("not a number"));
		profiler.SetMetric(ratio, Value::INTEGER(2));
		// a string is kept by name
		profiler.SetMetric(label, Value("hello"));
	};
	REQUIRE_NO_FAIL(con.Query(CHUNK_CALLBACK_QUERY));
	auto chunks = GetProfiledMetric(con, "test.fit_counter");
	REQUIRE(!chunks.IsNull());
	REQUIRE(chunks.GetValue<uint64_t>() >= 1);
	REQUIRE(chunks.GetValue<uint64_t>() < 100);
	REQUIRE(GetProfiledMetric(con, "test.fit_count") == Value::UBIGINT(3));
	REQUIRE(GetProfiledMetric(con, "test.fit_ratio") == Value::DOUBLE(2));
	REQUIRE(GetProfiledMetric(con, "test.fit_label") == Value("hello"));
}

TEST_CASE("Metrics are not updated by index while profiling is disabled", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto counter = MetricsManager::Get(*con.context)
	                   .RegisterMetric({"test.disabled_counter", "uint64", "A counter", "rows", MetricKind::COUNTER});
	SetUpChunkCallback(con, "test_disabled_output.txt");
	chunk_callback = [&](QueryProfiler &profiler, idx_t) {
		profiler.AddToMetric(counter, 1);
	};
	REQUIRE_NO_FAIL(con.Query(CHUNK_CALLBACK_QUERY));
	REQUIRE(!GetProfiledMetric(con, "test.disabled_counter").IsNull());

	// the counts of the last profiled query are kept, and the query that is not profiled adds nothing to them
	auto &query_metrics = QueryProfiler::Get(*con.context).GetQueryMetrics();
	auto profiled_count = query_metrics.GetStringCounter("test.disabled_counter");
	REQUIRE(profiled_count > 0);
	con.DisableProfiling();
	REQUIRE_NO_FAIL(con.Query(CHUNK_CALLBACK_QUERY));
	REQUIRE(query_metrics.GetStringCounter("test.disabled_counter") == profiled_count);
}

TEST_CASE("EXPLAIN ANALYZE reports a metric updated by index", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto counter = MetricsManager::Get(*con.context)
	                   .RegisterMetric({"test.explained_counter", "uint64", "A counter", "rows", MetricKind::COUNTER});
	SetUpChunkCallback(con, "test_explain_analyze_output.txt");
	chunk_callback = [&](QueryProfiler &profiler, idx_t size) {
		profiler.AddToMetric(counter, size);
	};
	REQUIRE_NO_FAIL(con.Query(string("EXPLAIN ANALYZE ") + CHUNK_CALLBACK_QUERY));
	REQUIRE(GetProfiledMetric(con, "test.explained_counter") == Value::UBIGINT(10000));
}

TEST_CASE("An index belongs to the database the metric was registered in", "[api]") {
	DuckDB first_db(nullptr);
	DuckDB second_db(nullptr);
	auto &first_manager = MetricsManager::Get(*first_db.instance);
	auto &second_manager = MetricsManager::Get(*second_db.instance);
	// registered in another order, so the same name gets another index
	auto first_a = first_manager.RegisterMetric({"test.db_a", "uint64", "A", "rows", MetricKind::COUNTER});
	auto first_b = first_manager.RegisterMetric({"test.db_b", "uint64", "B", "rows", MetricKind::COUNTER});
	auto second_b = second_manager.RegisterMetric({"test.db_b", "uint64", "B", "rows", MetricKind::COUNTER});
	auto second_a = second_manager.RegisterMetric({"test.db_a", "uint64", "A", "rows", MetricKind::COUNTER});
	REQUIRE(first_a.GetIndex() == second_b.GetIndex());
	REQUIRE(first_b.GetIndex() == second_a.GetIndex());

	Connection con(second_db);
	SetUpChunkCallback(con, "test_two_databases_output.txt");
	chunk_callback = [&](QueryProfiler &profiler, idx_t size) {
		profiler.AddToMetric(second_a, size);
	};
	REQUIRE_NO_FAIL(con.Query(CHUNK_CALLBACK_QUERY));
	REQUIRE(GetProfiledMetric(con, "test.db_a") == Value::UBIGINT(10000));
	REQUIRE(GetProfiledMetric(con, "test.db_b").IsNull());
}

TEST_CASE("An update from another thread may come while queries start and metrics are registered", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &metrics_manager = MetricsManager::Get(*con.context);
	auto counter =
	    metrics_manager.RegisterMetric({"test.late_counter", "uint64", "Counted late", "rows", MetricKind::COUNTER});
	auto value = metrics_manager.RegisterMetric({"test.late_value", "double", "Set late", "ratio"});
	SetUpChunkCallback(con, "test_late_updates_output.txt");
	chunk_callback = [&](QueryProfiler &, idx_t) {
	};

	// like an extension's background thread, which reports to the profiler of the connection that started it
	auto &profiler = QueryProfiler::Get(*con.context);
	atomic<bool> done {false};
	thread updater([&]() {
		while (!done) {
			profiler.AddToMetric(counter, 1);
			profiler.SetMetric(value, Value::DOUBLE(0.5));
			profiler.AddToMetricCounter("test.late_counter", 1);
		}
	});
	for (idx_t i = 0; i < 200; i++) {
		// every registration makes the next query start with new slots
		metrics_manager.RegisterMetric(
		    {"test.late_registered_" + to_string(i), "uint64", "Registered late", "rows", MetricKind::COUNTER});
		REQUIRE_NO_FAIL(con.Query("SELECT 42"));
	}
	done = true;
	updater.join();
}
