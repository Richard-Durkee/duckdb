#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/multi_file/multi_file_states.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

using namespace duckdb;

namespace {

OpenFileInfo FileWithSize(const string &path, idx_t size) {
	OpenFileInfo file(path);
	file.extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
	file.extended_info->options["file_size"] = Value::UBIGINT(size);
	return file;
}

vector<string> Paths(const MultiFileList &list) {
	vector<string> paths;
	for (auto &file : list.GetAllFiles()) {
		paths.push_back(file.path);
	}
	return paths;
}

optional_ptr<LogicalGet> FindGet(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		return &op.Cast<LogicalGet>();
	}
	for (auto &child : op.children) {
		auto get = FindGet(*child);
		if (get) {
			return get;
		}
	}
	return nullptr;
}

} // namespace

TEST_CASE("RestrictToFiles keeps the requested files in the list's order", "[multifile]") {
	SimpleMultiFileList list({OpenFileInfo("a.csv"), FileWithSize("b.csv", 42), OpenFileInfo("c.csv")});

	auto restricted = list.RestrictToFiles({OpenFileInfo("c.csv"), OpenFileInfo("b.csv"), OpenFileInfo("c.csv")});
	REQUIRE(Paths(*restricted) == vector<string> {"b.csv", "c.csv"});
	// the list's own file is kept, with the options it is opened with
	auto files = restricted->GetAllFiles();
	REQUIRE(files[0].extended_info);
	REQUIRE(files[0].extended_info->options["file_size"] == Value::UBIGINT(42));

	REQUIRE(list.RestrictToFiles({})->GetTotalFileCount() == 0);
	REQUIRE(Paths(list) == vector<string> {"a.csv", "b.csv", "c.csv"});

	REQUIRE_THROWS_WITH(list.RestrictToFiles({OpenFileInfo("a.csv"), OpenFileInfo("d.csv")}),
	                    Catch::Contains("d.csv") && Catch::Contains("it is not one of the list's files"));
}

TEST_CASE("A bound multi-file scan can be restricted to some of its files", "[multifile]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto dir = TestCreatePath("restrict_to_files");
	TestCreateDirectory(dir);
	for (idx_t i = 1; i <= 3; i++) {
		auto path = dir + "/part_" + to_string(i) + ".csv";
		REQUIRE_NO_FAIL(con.Query("COPY (SELECT " + to_string(i) + " AS x) TO '" + path + "' (HEADER true)"));
	}

	con.BeginTransaction();
	auto plan = con.context->ExtractPlan("SELECT sum(x) FROM read_csv('" + dir + "/*.csv')");
	auto get = FindGet(*plan);
	REQUIRE(get);
	auto &bind_data = get->bind_data->Cast<MultiFileBindData>();
	REQUIRE(bind_data.file_list->GetTotalFileCount() == 3);
	bind_data.file_list =
	    bind_data.file_list->RestrictToFiles({OpenFileInfo(dir + "/part_3.csv"), OpenFileInfo(dir + "/part_1.csv")});
	REQUIRE(Paths(*bind_data.file_list) == vector<string> {dir + "/part_1.csv", dir + "/part_3.csv"});
	plan.reset();
	con.Query("COMMIT");
}
