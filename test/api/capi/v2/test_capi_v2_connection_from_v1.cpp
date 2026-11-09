#include "test_capi_v2.hpp"

#include "duckdb.h"

#include <chrono>
#include <string>
#include <thread>

using namespace test_capi_v2;

// V2 connections on the database of a V1 duckdb_database, or on the session of a V1 duckdb_connection.

namespace {

idx_t V2Count(duckdb_v2_connection_handle conn, const std::string &sql) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto rows = DrainRowCount(result);
	duckdb_v2_result_destroy(&result);
	return rows;
}

void V1Exec(duckdb_connection conn, const char *sql) {
	duckdb_result result;
	REQUIRE(duckdb_query(conn, sql, &result) == DuckDBSuccess);
	duckdb_destroy_result(&result);
}

int64_t V1Int(duckdb_connection conn, const char *sql) {
	duckdb_result result;
	REQUIRE(duckdb_query(conn, sql, &result) == DuckDBSuccess);
	auto value = duckdb_value_int64(&result, 0, 0);
	duckdb_destroy_result(&result);
	return value;
}

} // namespace

TEST_CASE("V2: connection_create_from_v1_database", "[capi_v2][connection]") {
	duckdb_database v1_db = nullptr;
	duckdb_connection v1_conn = nullptr;
	REQUIRE(duckdb_open(nullptr, &v1_db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(v1_db, &v1_conn) == DuckDBSuccess);

	V1Exec(v1_conn, "CREATE TABLE t AS SELECT range AS i FROM range(5)");
	V1Exec(v1_conn, "ATTACH ':memory:' AS other");
	V1Exec(v1_conn, "CREATE TABLE other.t AS SELECT range AS i FROM range(3)");

	duckdb_v2_connection_handle conn = nullptr;
	REQUIRE(duckdb_v2_connection_create_from_v1_database(v1_db, &conn, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(conn);

	// the catalog and the attached databases are shared
	REQUIRE(V2Count(conn, "SELECT * FROM main.t") == 5);
	REQUIRE(V2Count(conn, "SELECT * FROM other.t") == 3);

	// writes through either connection are visible to the other
	V2Count(conn, "INSERT INTO main.t VALUES (5), (6)");
	REQUIRE(V1Int(v1_conn, "SELECT count(*) FROM main.t") == 7);

	// session settings are each connection's own
	V1Exec(v1_conn, "SET search_path = 'other'");
	REQUIRE(V1Int(v1_conn, "SELECT count(*) FROM t") == 3);
	REQUIRE(V2Count(conn, "SELECT * FROM t") == 7);
	V2Count(conn, "SET search_path = 'other'");
	REQUIRE(V2Count(conn, "SELECT * FROM t") == 3);
	V1Exec(v1_conn, "RESET search_path");
	REQUIRE(V1Int(v1_conn, "SELECT count(*) FROM t") == 7);
	REQUIRE(V2Count(conn, "SELECT * FROM t") == 3);

	// global settings are shared
	V1Exec(v1_conn, "SET GLOBAL threads = 3");
	REQUIRE(V2Count(conn, "SELECT 1 WHERE current_setting('threads') = 3") == 1);

	duckdb_v2_connection_destroy(&conn);
	REQUIRE(!conn);
	duckdb_disconnect(&v1_conn);
	duckdb_close(&v1_db);
}

TEST_CASE("V2: connection_create_from_v1_database outlives duckdb_close", "[capi_v2][connection]") {
	duckdb_database v1_db = nullptr;
	REQUIRE(duckdb_open(nullptr, &v1_db) == DuckDBSuccess);
	duckdb_v2_connection_handle conn = nullptr;
	REQUIRE(duckdb_v2_connection_create_from_v1_database(v1_db, &conn, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_close(&v1_db);

	V2Count(conn, "CREATE TABLE t AS SELECT range AS i FROM range(4)");
	REQUIRE(V2Count(conn, "SELECT * FROM t") == 4);
	duckdb_v2_connection_destroy(&conn);
}

TEST_CASE("V2: connection_create_from_v1_database null arguments", "[capi_v2][connection]") {
	duckdb_v2_connection_handle conn = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_connection_create_from_v1_database(nullptr, &conn, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(err);
	duckdb_v2_error_info_destroy(&err);

	duckdb_database v1_db = nullptr;
	REQUIRE(duckdb_open(nullptr, &v1_db) == DuckDBSuccess);
	REQUIRE(duckdb_v2_connection_create_from_v1_database(v1_db, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_close(&v1_db);
}

TEST_CASE("V2: connection_create_from_v1_connection shares the session", "[capi_v2][connection]") {
	duckdb_database v1_db = nullptr;
	duckdb_connection v1_conn = nullptr;
	REQUIRE(duckdb_open(nullptr, &v1_db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(v1_db, &v1_conn) == DuckDBSuccess);

	V1Exec(v1_conn, "CREATE TABLE t AS SELECT range AS i FROM range(5)");
	V1Exec(v1_conn, "ATTACH ':memory:' AS other");
	V1Exec(v1_conn, "CREATE TABLE other.t AS SELECT range AS i FROM range(3)");
	V1Exec(v1_conn, "CREATE TEMP TABLE tmp AS SELECT 1 AS j");
	V1Exec(v1_conn, "SET search_path = 'other'");

	duckdb_v2_connection_handle conn = nullptr;
	REQUIRE(duckdb_v2_connection_create_from_v1_connection(v1_conn, &conn, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(conn);

	// session settings and temporary tables are the V1 connection's
	REQUIRE(V2Count(conn, "SELECT * FROM t") == 3);
	REQUIRE(V2Count(conn, "SELECT * FROM tmp") == 1);
	V2Count(conn, "RESET search_path");
	REQUIRE(V1Int(v1_conn, "SELECT count(*) FROM t") == 5);

	// so is the open transaction
	V1Exec(v1_conn, "BEGIN");
	V2Count(conn, "INSERT INTO t VALUES (5)");
	REQUIRE(V1Int(v1_conn, "SELECT count(*) FROM t") == 6);
	V1Exec(v1_conn, "ROLLBACK");
	REQUIRE(V2Count(conn, "SELECT * FROM t") == 5);

	// destroying the handle leaves the V1 connection open
	duckdb_v2_connection_destroy(&conn);
	REQUIRE(!conn);
	REQUIRE(V1Int(v1_conn, "SELECT count(*) FROM tmp") == 1);

	duckdb_disconnect(&v1_conn);
	duckdb_close(&v1_db);
}

TEST_CASE("V2: connection_create_from_v1_connection with an open result", "[capi_v2][connection]") {
	duckdb_database v1_db = nullptr;
	duckdb_connection v1_conn = nullptr;
	REQUIRE(duckdb_open(nullptr, &v1_db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(v1_db, &v1_conn) == DuckDBSuccess);
	duckdb_v2_connection_handle conn = nullptr;
	REQUIRE(duckdb_v2_connection_create_from_v1_connection(v1_conn, &conn, nullptr) == DUCKDB_V2_ERROR_NONE);

	// a V1 statement can't run while a V2 result holds the session
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, "SELECT * FROM range(100000)", &result, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(result);
	REQUIRE(chunk);
	duckdb_result v1_result;
	REQUIRE(duckdb_query(v1_conn, "SELECT 42", &v1_result) == DuckDBError);
	REQUIRE(std::string(duckdb_result_error(&v1_result)).find("open result") != std::string::npos);
	duckdb_destroy_result(&v1_result);
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&result);
	REQUIRE(V1Int(v1_conn, "SELECT 42") == 42);

	duckdb_v2_connection_destroy(&conn);
	duckdb_disconnect(&v1_conn);
	duckdb_close(&v1_db);
}

TEST_CASE("V2: connection_create_from_v1_connection with an open V1 result", "[capi_v2][connection]") {
	duckdb_database v1_db = nullptr;
	duckdb_connection v1_conn = nullptr;
	REQUIRE(duckdb_open(nullptr, &v1_db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(v1_db, &v1_conn) == DuckDBSuccess);
	duckdb_v2_connection_handle conn = nullptr;
	REQUIRE(duckdb_v2_connection_create_from_v1_connection(v1_conn, &conn, nullptr) == DUCKDB_V2_ERROR_NONE);

	// a V2 statement can't run while a V1 streaming result holds the session
	duckdb_prepared_statement prepared = nullptr;
	REQUIRE(duckdb_prepare(v1_conn, "SELECT * FROM range(100000)", &prepared) == DuckDBSuccess);
	duckdb_pending_result pending = nullptr;
	REQUIRE(duckdb_pending_prepared_streaming(prepared, &pending) == DuckDBSuccess);
	duckdb_result v1_result;
	REQUIRE(duckdb_execute_pending(pending, &v1_result) == DuckDBSuccess);
	REQUIRE(RunSql(conn, "SELECT 42") != DUCKDB_V2_ERROR_NONE);
	duckdb_destroy_result(&v1_result);
	duckdb_destroy_pending(&pending);
	duckdb_destroy_prepare(&prepared);
	REQUIRE(V2Count(conn, "SELECT 42") == 1);

	duckdb_v2_connection_destroy(&conn);
	duckdb_disconnect(&v1_conn);
	duckdb_close(&v1_db);
}

TEST_CASE("V2: connection_create_from_v1_connection interrupted through V1", "[capi_v2][connection]") {
	duckdb_database v1_db = nullptr;
	duckdb_connection v1_conn = nullptr;
	REQUIRE(duckdb_open(nullptr, &v1_db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(v1_db, &v1_conn) == DuckDBSuccess);
	duckdb_v2_connection_handle conn = nullptr;
	REQUIRE(duckdb_v2_connection_create_from_v1_connection(v1_conn, &conn, nullptr) == DUCKDB_V2_ERROR_NONE);

	// a V1 interrupt stops the V2 statement, which fails with an error rather than reporting a cancel
	duckdb_v2_result_handle result = nullptr;
	V1Exec(v1_conn, "SET max_execution_time = 60000");
	REQUIRE(Query(conn, "SELECT count(*) FROM range(1000000000) a, range(1000) b", &result, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	std::thread interrupter([&]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		duckdb_interrupt(v1_conn);
	});
	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	duckdb_v2_error_info_handle err = nullptr;
	DUCKDB_V2_ERROR rc = DUCKDB_V2_ERROR_NONE;
	while (rc == DUCKDB_V2_ERROR_NONE && status == DUCKDB_V2_RESULT_STEP_STATUS_WAITING) {
		rc = duckdb_v2_result_step(result, &chunk, &status, &err);
		if (rc == DUCKDB_V2_ERROR_NONE && status == DUCKDB_V2_RESULT_STEP_STATUS_WAITING) {
			rc = duckdb_v2_result_wait(result, nullptr);
		}
	}
	interrupter.join();
	REQUIRE(rc != DUCKDB_V2_ERROR_NONE);
	REQUIRE(status != DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED);
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&result);
	REQUIRE(V1Int(v1_conn, "SELECT 42") == 42);

	duckdb_v2_connection_destroy(&conn);
	duckdb_disconnect(&v1_conn);
	duckdb_close(&v1_db);
}

TEST_CASE("V2: connection_create_from_v1_connection null arguments", "[capi_v2][connection]") {
	duckdb_v2_connection_handle conn = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_connection_create_from_v1_connection(nullptr, &conn, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(err);
	duckdb_v2_error_info_destroy(&err);

	duckdb_database v1_db = nullptr;
	duckdb_connection v1_conn = nullptr;
	REQUIRE(duckdb_open(nullptr, &v1_db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(v1_db, &v1_conn) == DuckDBSuccess);
	REQUIRE(duckdb_v2_connection_create_from_v1_connection(v1_conn, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_disconnect(&v1_conn);
	duckdb_close(&v1_db);
}
