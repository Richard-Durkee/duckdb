#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

struct DuckDBOperatorMemoryData : public GlobalTableFunctionState {
	DuckDBOperatorMemoryData() : offset(0) {
	}

	vector<OperatorMemoryInformation> entries;
	idx_t offset;
};

static unique_ptr<FunctionData> DuckDBOperatorMemoryBind(ClientContext &context, TableFunctionBindInput &input,
                                                         vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("connection_id");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("query_id");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("operator_id");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("operator_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("memory_usage_bytes");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("peak_memory_usage_bytes");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("memory_usage_bytes_by_tag");
	return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::BIGINT));

	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> DuckDBOperatorMemoryInit(ClientContext &context,
                                                                     TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBOperatorMemoryData>();
	result->entries = BufferManager::GetBufferManager(context).GetBufferPool().GetOperatorMemorySnapshot();
	return std::move(result);
}

static Value OptionalIndexValue(idx_t value) {
	return value == DConstants::INVALID_INDEX ? Value(LogicalType::UBIGINT) : Value::UBIGINT(value);
}

static void DuckDBOperatorMemoryFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBOperatorMemoryData>();
	idx_t count = 0;
	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset++];
		auto &identity = entry.identity;
		output.data[0].Append(OptionalIndexValue(identity.connection_id));
		output.data[1].Append(OptionalIndexValue(identity.query_id));
		output.data[2].Append(OptionalIndexValue(identity.operator_id));
		output.data[3].Append(Value(identity.operator_name));
		output.data[4].Append(Value::BIGINT(ClampReportedMemory(entry.memory_usage_bytes)));
		output.data[5].Append(Value::BIGINT(ClampReportedMemory(entry.peak_memory_usage_bytes)));
		vector<Value> tags;
		vector<Value> tag_bytes;
		for (idx_t tag_idx = 0; tag_idx < MEMORY_TAG_COUNT; tag_idx++) {
			auto bytes = entry.memory_usage_bytes_per_tag[tag_idx];
			if (bytes == 0) {
				continue;
			}
			tags.emplace_back(EnumUtil::ToString(static_cast<MemoryTag>(tag_idx)));
			tag_bytes.push_back(Value::BIGINT(ClampReportedMemory(bytes)));
		}
		output.data[6].Append(
		    Value::MAP(LogicalType::VARCHAR, LogicalType::BIGINT, std::move(tags), std::move(tag_bytes)));
		count++;
	}
}

void DuckDBOperatorMemoryFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("duckdb_operator_memory", {}, DuckDBOperatorMemoryFunction, DuckDBOperatorMemoryBind,
	                              DuckDBOperatorMemoryInit));
}

} // namespace duckdb
