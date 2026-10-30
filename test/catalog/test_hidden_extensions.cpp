#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_manager.hpp"

using namespace duckdb;

static void HiddenTestFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(Value::INTEGER(42), count_t(args.size()));
}

static idx_t CountRows(Connection &con, const string &query) {
	auto result = con.Query(query);
	REQUIRE_NO_FAIL(*result);
	return result->RowCount();
}

TEST_CASE("Hidden extensions are left out of the system catalog views", "[catalog]") {
	DuckDB db(nullptr);
	Connection con(db);

	// register what an extension typically adds while it loads
	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "hidden_test_extension", ""};
	ExtensionLoader loader {load_info};
	{
		ExtensionLoadScope load_scope(loader.GetRegisteredExtensionName());
		loader.RegisterFunction(ScalarFunction("hidden_test_function", {}, LogicalType::INTEGER, HiddenTestFunction));
		DBConfig::GetConfig(*db.instance)
		    .AddExtensionOption("hidden_test_option", "A setting of the hidden extension", LogicalType::BIGINT,
		                        Value::BIGINT(1));
		Connection load_con(db);
		REQUIRE_NO_FAIL(load_con.Query("CREATE SCHEMA hidden_test_schema"));
		REQUIRE_NO_FAIL(load_con.Query("CREATE TABLE hidden_test_schema.hidden_test_table (i INTEGER)"));
		REQUIRE_NO_FAIL(load_con.Query("CREATE VIEW hidden_test_view AS SELECT 1 AS i"));
	}
	// created after the load: not attributed to the extension
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE user_table (i INTEGER)"));

	const vector<string> listings {
	    "SELECT * FROM duckdb_functions() WHERE function_name = 'hidden_test_function'",
	    "SELECT * FROM duckdb_settings() WHERE name = 'hidden_test_option'",
	    "SELECT * FROM duckdb_schemas() WHERE schema_name = 'hidden_test_schema'",
	    "SELECT * FROM duckdb_tables() WHERE table_name = 'hidden_test_table'",
	    "SELECT * FROM duckdb_columns() WHERE table_name = 'hidden_test_table'",
	    "SELECT * FROM duckdb_views() WHERE view_name = 'hidden_test_view'",
	    "SELECT * FROM information_schema.tables WHERE table_name IN ('hidden_test_table', 'hidden_test_view')",
	};
	for (auto &listing : listings) {
		REQUIRE(CountRows(con, listing) > 0);
	}

	REQUIRE_NO_FAIL(con.Query("SET hidden_extensions = ['Hidden_Test_Extension']"));
	for (auto &listing : listings) {
		INFO(listing);
		REQUIRE(CountRows(con, listing) == 0);
	}
	REQUIRE(CountRows(con, "SELECT * FROM duckdb_tables() WHERE table_name = 'user_table'") == 1);

	// hidden entries can still be used
	auto result = con.Query("SELECT hidden_test_function()");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	result = con.Query("SELECT current_setting('hidden_test_option')");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE_NO_FAIL(con.Query("SELECT * FROM hidden_test_schema.hidden_test_table"));

	// and are not suggested
	result = con.Query("SELECT hidden_test_functio()");
	REQUIRE(result->HasError());
	REQUIRE(result->GetError().find("hidden_test_function") == string::npos);
	result = con.Query("SET hidden_test_optio = 2");
	REQUIRE(result->HasError());
	REQUIRE(result->GetError().find("hidden_test_option") == string::npos);

	REQUIRE_NO_FAIL(con.Query("RESET hidden_extensions"));
	for (auto &listing : listings) {
		REQUIRE(CountRows(con, listing) > 0);
	}
	result = con.Query("SELECT hidden_test_functio()");
	REQUIRE(result->HasError());
	REQUIRE(result->GetError().find("hidden_test_function") != string::npos);
}
