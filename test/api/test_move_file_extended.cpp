#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/open_file_info.hpp"

using namespace duckdb;

namespace {

string MoveProbePath(const string &name) {
	LocalFileSystem fs;
	return fs.JoinPath(TestDirectoryPath(), name);
}

void WriteProbeFile(FileSystem &fs, const string &path) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
	handle->Write((void *)"abc", 3);
	handle->Close();
}

} // namespace

TEST_CASE("Test MoveFileExtended on a file system that cannot check a version", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &fs = FileSystem::GetFileSystem(*con.context);
	fs.CreateDirectoriesRecursive(TestDirectoryPath());
	auto source = MoveProbePath("mfe_source.txt");
	auto target = MoveProbePath("mfe_target.txt");
	fs.TryRemoveFile(source);
	fs.TryRemoveFile(target);

	SECTION("without options it moves like MoveFile") {
		WriteProbeFile(fs, source);
		fs.MoveFileExtended(OpenFileInfo(source), target);
		REQUIRE(!fs.FileExists(source));
		REQUIRE(fs.FileExists(target));
	}

	SECTION("a file size alone does not stop the move") {
		WriteProbeFile(fs, source);
		OpenFileInfo info(source);
		info.extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
		info.extended_info->options["file_size"] = Value::UBIGINT(3);
		fs.MoveFileExtended(info, target);
		REQUIRE(!fs.FileExists(source));
		REQUIRE(fs.FileExists(target));
	}

	SECTION("an expected version it cannot check is refused and the source stays") {
		WriteProbeFile(fs, source);
		OpenFileInfo info(source);
		info.extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
		info.extended_info->options["etag"] = Value("\"0123\"");
		REQUIRE_THROWS_AS(fs.MoveFileExtended(info, target), NotImplementedException);
		REQUIRE(fs.FileExists(source));
		REQUIRE(!fs.FileExists(target));
	}
	fs.TryRemoveFile(source);
	fs.TryRemoveFile(target);
}
