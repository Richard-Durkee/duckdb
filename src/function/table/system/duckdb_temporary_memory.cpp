#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/temporary_memory_manager.hpp"

namespace duckdb {

struct DuckDBTemporaryMemoryData : public GlobalTableFunctionState {
	DuckDBTemporaryMemoryData() : offset(0) {
	}

	vector<TemporaryMemoryStateInformation> entries;
	idx_t offset;
};

static unique_ptr<FunctionData> DuckDBTemporaryMemoryBind(ClientContext &context, TableFunctionBindInput &input,
                                                          vector<LogicalType> &return_types,
                                                          vector<Identifier> &names) {
	names.emplace_back("operator_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("connection_id");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("query_id");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("reservation_bytes");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("remaining_size_bytes");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("minimum_reservation_bytes");
	return_types.emplace_back(LogicalType::BIGINT);

	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> DuckDBTemporaryMemoryInit(ClientContext &context,
                                                                      TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBTemporaryMemoryData>();
	result->entries = BufferManager::GetBufferManager(context).GetTemporaryMemoryManager().GetStateInformation();
	return std::move(result);
}

static Value OptionalIndexValue(idx_t value) {
	return value == DConstants::INVALID_INDEX ? Value(LogicalType::UBIGINT) : Value::UBIGINT(value);
}

static void DuckDBTemporaryMemoryFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBTemporaryMemoryData>();
	idx_t count = 0;
	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset++];
		output.data[0].Append(Value(entry.label));
		output.data[1].Append(OptionalIndexValue(entry.connection_id));
		output.data[2].Append(OptionalIndexValue(entry.query_id));
		output.data[3].Append(Value::BIGINT(ClampReportedMemory(entry.reservation)));
		output.data[4].Append(Value::BIGINT(ClampReportedMemory(entry.remaining_size)));
		output.data[5].Append(Value::BIGINT(ClampReportedMemory(entry.minimum_reservation)));
		count++;
	}
}

void DuckDBTemporaryMemoryFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("duckdb_temporary_memory", {}, DuckDBTemporaryMemoryFunction,
	                              DuckDBTemporaryMemoryBind, DuckDBTemporaryMemoryInit));
}

} // namespace duckdb
