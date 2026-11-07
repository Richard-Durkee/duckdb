#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/main/query_written_files.hpp"

namespace duckdb {

struct DuckDBLastQueryWrittenFilesData : public GlobalTableFunctionState {
	vector<string> files;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> DuckDBLastQueryWrittenFilesBind(ClientContext &context, TableFunctionBindInput &input,
                                                                vector<LogicalType> &return_types,
                                                                vector<Identifier> &names) {
	names.emplace_back("file_path");
	return_types.emplace_back(LogicalType::VARCHAR);
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> DuckDBLastQueryWrittenFilesInit(ClientContext &context,
                                                                            TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBLastQueryWrittenFilesData>();
	result->files = QueryWrittenFiles::Get(context).ReadLastQueryFiles();
	return std::move(result);
}

static void DuckDBLastQueryWrittenFilesFunction(ClientContext &context, TableFunctionInput &data_p,
                                                DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBLastQueryWrittenFilesData>();
	idx_t count = 0;
	while (data.offset < data.files.size() && count < STANDARD_VECTOR_SIZE) {
		output.data[0].Append(Value(data.files[data.offset++]));
		count++;
	}
}

void DuckDBLastQueryWrittenFilesFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("duckdb_last_query_written_files", {}, DuckDBLastQueryWrittenFilesFunction,
	                              DuckDBLastQueryWrittenFilesBind, DuckDBLastQueryWrittenFilesInit));
}

} // namespace duckdb
