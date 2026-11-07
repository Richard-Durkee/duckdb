#include "duckdb/main/query_written_files.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

QueryWrittenFiles &QueryWrittenFiles::Get(ClientContext &context) {
	return *context.registered_state->GetOrCreate<QueryWrittenFiles>(STATE_KEY);
}

void QueryWrittenFiles::QueryBegin(ClientContext &context) {
	lock_guard<mutex> guard(lock);
	current_query_files.clear();
	reading_last_query = false;
}

void QueryWrittenFiles::QueryEnd(ClientContext &context, optional_ptr<ErrorData> error) {
	lock_guard<mutex> guard(lock);
	if (!reading_last_query) {
		// a failed query's files are not part of the table
		if (error && error->HasError()) {
			last_query_files.clear();
		} else {
			last_query_files = std::move(current_query_files);
		}
	}
	current_query_files.clear();
	reading_last_query = false;
}

void QueryWrittenFiles::AddFiles(vector<string> files) {
	lock_guard<mutex> guard(lock);
	for (auto &file : files) {
		current_query_files.push_back(std::move(file));
	}
}

vector<string> QueryWrittenFiles::ReadLastQueryFiles() {
	lock_guard<mutex> guard(lock);
	reading_last_query = true;
	return last_query_files;
}

} // namespace duckdb
