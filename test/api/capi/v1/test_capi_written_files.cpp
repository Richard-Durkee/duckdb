#include "capi_tester.hpp"
#include "duckdb.hpp"

#include <algorithm>

using namespace duckdb;

static vector<string> WrittenFiles(duckdb_result &result) {
	vector<string> files;
	for (idx_t i = 0; i < duckdb_result_written_file_count(&result); i++) {
		files.push_back(duckdb_result_written_file(&result, i));
	}
	std::sort(files.begin(), files.end());
	return files;
}

TEST_CASE("Test the files a query wrote in the C API", "[capi]") {
	duckdb_database db;
	duckdb_connection con;
	duckdb_result result;
	REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(db, &con) == DuckDBSuccess);

	// a COPY that returns only its row count
	auto plain = TestCreatePath("capi_written_files_plain.parquet");
	REQUIRE(duckdb_query(con, ("COPY (SELECT range AS i FROM range(10)) TO '" + plain + "'").c_str(), &result) ==
	        DuckDBSuccess);
	REQUIRE(WrittenFiles(result) == vector<string> {plain});
	REQUIRE(duckdb_result_written_file(&result, 1) == nullptr);
	duckdb_destroy_result(&result);

	// a partitioned COPY reports every file it wrote
	auto partitioned = TestCreatePath("capi_written_files_partitioned");
	REQUIRE(duckdb_query(con,
	                     ("COPY (SELECT range % 3 AS p, range AS i FROM range(30)) TO '" + partitioned +
	                      "' (FORMAT parquet, PARTITION_BY (p))")
	                         .c_str(),
	                     &result) == DuckDBSuccess);
	auto files = WrittenFiles(result);
	REQUIRE(files.size() == 3);
	for (idx_t p = 0; p < 3; p++) {
		REQUIRE(files[p] == partitioned + "/p=" + std::to_string(p) + "/data_0.parquet");
	}
	duckdb_destroy_result(&result);

	// the final name of a file written through a temporary one
	auto tmp_target = TestCreatePath("tmp_capi_written_files.parquet");
	REQUIRE(duckdb_query(con,
	                     ("COPY (SELECT 1 AS i) TO '" + tmp_target + "' (USE_TMP_FILE true, RETURN_STATS)").c_str(),
	                     &result) == DuckDBSuccess);
	REQUIRE(WrittenFiles(result) == vector<string> {tmp_target});
	duckdb_destroy_result(&result);

	// a query that writes nothing, and a failing one
	REQUIRE(duckdb_query(con, "SELECT 42", &result) == DuckDBSuccess);
	REQUIRE(duckdb_result_written_file_count(&result) == 0);
	duckdb_destroy_result(&result);
	REQUIRE(duckdb_query(
	            con,
	            ("COPY (SELECT error('boom')) TO '" + TestCreatePath("capi_written_files_fails.parquet") + "'").c_str(),
	            &result) == DuckDBError);
	REQUIRE(duckdb_result_written_file_count(&result) == 0);
	duckdb_destroy_result(&result);

	REQUIRE(duckdb_result_written_file_count(nullptr) == 0);
	REQUIRE(duckdb_result_written_file(nullptr, 0) == nullptr);

	duckdb_disconnect(&con);
	duckdb_close(&db);
}

TEST_CASE("Test the files a query wrote in the C++ API", "[capi]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto file = TestCreatePath("cpp_written_files.parquet");
	auto result = con.Query("COPY (SELECT range AS i FROM range(10)) TO '" + file + "'");
	REQUIRE(!result->HasError());
	REQUIRE(result->GetWrittenFiles() == vector<string> {file});

	// a table that is not written as files has none
	REQUIRE(!con.Query("CREATE TABLE t (i INTEGER)")->HasError());
	result = con.Query("INSERT INTO t SELECT range FROM range(10)");
	REQUIRE(!result->HasError());
	REQUIRE(result->GetWrittenFiles().empty());
}
