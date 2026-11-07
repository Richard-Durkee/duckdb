//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/query_written_files.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/client_context_state.hpp"

namespace duckdb {

//! The files the copies of a connection's query wrote. Once the query has succeeded, they become the files of its
//! most recent query, which duckdb_last_query_written_files() returns.
class QueryWrittenFiles : public ClientContextState {
public:
	static constexpr const char *STATE_KEY = "query_written_files";
	static QueryWrittenFiles &Get(ClientContext &context);

	void QueryBegin(ClientContext &context) override;
	void QueryEnd(ClientContext &context, optional_ptr<ErrorData> error) override;

	void AddFiles(vector<string> files);
	//! Returns the files of the most recent query, and keeps them past the end of the query that reads them
	vector<string> ReadLastQueryFiles();

private:
	mutex lock;
	vector<string> current_query_files;
	vector<string> last_query_files;
	bool reading_last_query = false;
};

} // namespace duckdb
