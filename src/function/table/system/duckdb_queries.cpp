#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

struct DuckDBQueriesEntry {
	connection_t connection_id;
	QueryActivity activity;
	double progress;
	idx_t rows_processed;
	idx_t total_rows_to_process;
};

struct DuckDBQueriesData : public GlobalTableFunctionState {
	vector<DuckDBQueriesEntry> entries;
	timestamp_t now;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> DuckDBQueriesBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("connection_id");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("state");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("query_id");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("query");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("query_start");
	return_types.emplace_back(LogicalType::TIMESTAMP_TZ);

	names.emplace_back("elapsed");
	return_types.emplace_back(LogicalType::INTERVAL);

	names.emplace_back("transaction_start");
	return_types.emplace_back(LogicalType::TIMESTAMP_TZ);

	names.emplace_back("progress");
	return_types.emplace_back(LogicalType::DOUBLE);

	names.emplace_back("rows_processed");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("total_rows_to_process");
	return_types.emplace_back(LogicalType::UBIGINT);
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> DuckDBQueriesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBQueriesData>();
	result->now = Timestamp::GetCurrentTimestamp();
	auto &conn_manager = context.db->GetConnectionManager();
	for (auto &connection : conn_manager.GetConnectionList()) {
		DuckDBQueriesEntry entry;
		entry.connection_id = connection->GetConnectionId();
		entry.activity = connection->GetQueryActivity();
		auto progress = connection->GetQueryProgress();
		entry.progress = progress.GetPercentage();
		entry.rows_processed = progress.GetRowsProcessed();
		entry.total_rows_to_process = progress.GetTotalRowsToProcess();
		result->entries.push_back(std::move(entry));
	}
	std::sort(
	    result->entries.begin(), result->entries.end(),
	    [](const DuckDBQueriesEntry &a, const DuckDBQueriesEntry &b) { return a.connection_id < b.connection_id; });
	return std::move(result);
}

static string QueryActivityStateToString(QueryActivityState state) {
	switch (state) {
	case QueryActivityState::IDLE:
		return "idle";
	case QueryActivityState::IDLE_IN_TRANSACTION:
		return "idle in transaction";
	case QueryActivityState::RUNNING:
		return "running";
	case QueryActivityState::WAITING_FOR_CLIENT:
		return "waiting for client";
	default:
		throw InternalException("Unrecognized QueryActivityState");
	}
}

static Value TimestampOrNull(bool valid, timestamp_t timestamp) {
	return valid ? Value::TIMESTAMPTZ(timestamp_tz_t(timestamp)) : Value(LogicalType::TIMESTAMP_TZ);
}

static void DuckDBQueriesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBQueriesData>();
	idx_t count = 0;
	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset++];
		auto &activity = entry.activity;
		output.data[0].Append(Value::UBIGINT(entry.connection_id));
		output.data[1].Append(Value(QueryActivityStateToString(activity.state)));
		output.data[2].Append(activity.running ? Value::UBIGINT(activity.query_id) : Value(LogicalType::UBIGINT));
		output.data[3].Append(activity.running ? Value(activity.query) : Value(LogicalType::VARCHAR));
		output.data[4].Append(TimestampOrNull(activity.running, activity.start_time));
		if (activity.running) {
			auto elapsed_micros = MaxValue<int64_t>(
			    Timestamp::GetEpochMicroSeconds(data.now) - Timestamp::GetEpochMicroSeconds(activity.start_time), 0);
			output.data[5].Append(Value::INTERVAL(Interval::FromMicro(elapsed_micros)));
		} else {
			output.data[5].Append(Value(LogicalType::INTERVAL));
		}
		output.data[6].Append(TimestampOrNull(activity.in_transaction, activity.transaction_start));
		if (!activity.running) {
			for (idx_t col = 7; col < output.ColumnCount(); col++) {
				output.data[col].Append(Value(output.data[col].GetType()));
			}
			count++;
			continue;
		}
		// progress is only tracked while the progress bar is enabled, and reported as -1 otherwise
		if (entry.progress < 0) {
			output.data[7].Append(Value(LogicalType::DOUBLE));
			output.data[8].Append(Value(LogicalType::UBIGINT));
			output.data[9].Append(Value(LogicalType::UBIGINT));
		} else {
			output.data[7].Append(Value::DOUBLE(entry.progress));
			output.data[8].Append(Value::UBIGINT(entry.rows_processed));
			output.data[9].Append(Value::UBIGINT(entry.total_rows_to_process));
		}
		count++;
	}
}

void DuckDBQueriesFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("duckdb_queries", {}, DuckDBQueriesFunction, DuckDBQueriesBind, DuckDBQueriesInit));
}

} // namespace duckdb
