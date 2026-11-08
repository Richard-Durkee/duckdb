#include "test_capi_v2.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace test_capi_v2;

// The files the COPY operators of a statement wrote, read off its finished result.

namespace {

std::vector<std::string> WrittenFiles(duckdb_v2_result_handle result) {
	idx_t count = 0;
	REQUIRE(duckdb_v2_result_get_written_file_count(result, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	std::vector<std::string> files;
	for (idx_t i = 0; i < count; i++) {
		const char *path = nullptr;
		idx_t length = 0;
		REQUIRE(duckdb_v2_result_get_written_file(result, i, &path, &length, nullptr) == DUCKDB_V2_ERROR_NONE);
		files.emplace_back(path, length);
	}
	std::sort(files.begin(), files.end());
	return files;
}

// Runs a statement to the end and returns its written files.
std::vector<std::string> DrainWrittenFiles(duckdb_v2_connection_handle conn, const std::string &sql) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result, nullptr) == DUCKDB_V2_ERROR_NONE);
	idx_t rows_changed = 0;
	REQUIRE(duckdb_v2_result_drain(result, &rows_changed, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto files = WrittenFiles(result);
	duckdb_v2_result_destroy(&result);
	return files;
}

} // namespace

TEST_CASE("V2: the files a statement wrote", "[capi_v2][query_result]") {
	EnvFixture fx;

	// a COPY that returns only its row count
	auto plain = TestCreatePath("v2_written_files_plain.parquet");
	REQUIRE(DrainWrittenFiles(fx.conn, "COPY (SELECT range AS i FROM range(10)) TO '" + plain + "'") ==
	        std::vector<std::string> {plain});

	// a partitioned COPY reports every file it wrote
	auto partitioned = TestCreatePath("v2_written_files_partitioned");
	auto files = DrainWrittenFiles(fx.conn, "COPY (SELECT range % 3 AS p, range AS i FROM range(30)) TO '" +
	                                            partitioned + "' (FORMAT parquet, PARTITION_BY (p))");
	REQUIRE(files.size() == 3);
	for (idx_t p = 0; p < 3; p++) {
		REQUIRE(files[p] == partitioned + "/p=" + std::to_string(p) + "/data_0.parquet");
	}

	// the final name of a file written through a temporary one, also when the name starts like a temporary one
	auto tmp_target = TestCreatePath("tmp_v2_written_files.parquet");
	REQUIRE(
	    DrainWrittenFiles(fx.conn, "COPY (SELECT 1 AS i) TO '" + tmp_target + "' (USE_TMP_FILE true, RETURN_STATS)") ==
	    std::vector<std::string> {tmp_target});

	// a COPY that writes a file per thread, and one that rotates files
	REQUIRE(DrainWrittenFiles(fx.conn, "SET threads = 4").empty());
	auto per_thread = TestCreatePath("v2_written_files_per_thread");
	REQUIRE(!DrainWrittenFiles(fx.conn, "COPY (SELECT range AS i FROM range(1000000)) TO '" + per_thread +
	                                        "' (FORMAT parquet, PER_THREAD_OUTPUT)")
	             .empty());
	auto rotated = TestCreatePath("v2_written_files_rotated");
	REQUIRE(DrainWrittenFiles(fx.conn, "COPY (SELECT range AS i FROM range(100000)) TO '" + rotated +
	                                       "' (FORMAT parquet, FILE_SIZE_BYTES '100kb', ROW_GROUP_SIZE 10000)")
	            .size() > 1);

	// statements that write no files
	REQUIRE(DrainWrittenFiles(fx.conn, "SELECT 42").empty());
	REQUIRE(DrainWrittenFiles(fx.conn, "CREATE TABLE t AS SELECT range AS i FROM range(10)").empty());
	REQUIRE(DrainWrittenFiles(fx.conn, "INSERT INTO t SELECT range FROM range(10)").empty());
}

TEST_CASE("V2: written files of a statement in a transaction", "[capi_v2][query_result]") {
	EnvFixture fx;
	auto file = TestCreatePath("v2_written_files_transaction.parquet");
	REQUIRE(DrainWrittenFiles(fx.conn, "BEGIN").empty());
	// the statement's files, though its transaction is rolled back later
	REQUIRE(DrainWrittenFiles(fx.conn, "COPY (SELECT 1 AS i) TO '" + file + "'") == std::vector<std::string> {file});
	REQUIRE(DrainWrittenFiles(fx.conn, "ROLLBACK").empty());
}

TEST_CASE("V2: written files edge and error paths", "[capi_v2][query_result]") {
	EnvFixture fx;

	// not known before the result is finished
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(fx.conn,
	              "COPY (SELECT range AS i FROM range(10)) TO '" +
	                  TestCreatePath("v2_written_files_unfinished.parquet") + "'",
	              &result, nullptr) == DUCKDB_V2_ERROR_NONE);
	idx_t count = 0;
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_result_get_written_file_count(result, &count, &err) != DUCKDB_V2_ERROR_NONE);
	duckdb_v2_error_info_destroy(&err);
	idx_t rows_changed = 0;
	REQUIRE(duckdb_v2_result_drain(result, &rows_changed, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_result_get_written_file_count(result, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(count == 1);

	// an index out of range
	const char *path = nullptr;
	idx_t length = 0;
	REQUIRE(duckdb_v2_result_get_written_file(result, 1, &path, &length, &err) != DUCKDB_V2_ERROR_NONE);
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_result_destroy(&result);

	// a COPY that fails writes none
	REQUIRE(Query(fx.conn,
	              "COPY (SELECT range % 2 AS p, CASE WHEN range = 5000 THEN error('boom') ELSE range END AS i FROM "
	              "range(10000)) TO '" +
	                  TestCreatePath("v2_written_files_failed") + "' (FORMAT parquet, PARTITION_BY (p))",
	              &result, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_result_drain(result, &rows_changed, &err) != DUCKDB_V2_ERROR_NONE);
	duckdb_v2_error_info_destroy(&err);
	REQUIRE(duckdb_v2_result_get_written_file_count(result, &count, &err) != DUCKDB_V2_ERROR_NONE);
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_result_destroy(&result);

	// null arguments
	REQUIRE(duckdb_v2_result_get_written_file_count(nullptr, &count, nullptr) != DUCKDB_V2_ERROR_NONE);
}
