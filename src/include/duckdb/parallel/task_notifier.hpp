//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/task_notifier.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/client_context_state.hpp"

namespace duckdb {
class ClientContext;
class ExecutorTask;

//! The TaskNotifier notifies ClientContextState listeners about started / stopped tasks
//! The listeners are captured on start, so every state that sees OnTaskStart also sees OnTaskStop
class TaskNotifier {
public:
	TaskNotifier(optional_ptr<ClientContext> context_p, const Task &task);
	TaskNotifier(ClientContext &context_p, const ExecutorTask &task);

	~TaskNotifier();

public:
	//! Sets the result passed to OnTaskStop - if never set, the task is reported as TASK_ERROR
	void SetResult(TaskExecutionResult result) {
		info.result = result;
	}

private:
	bool CaptureListeners();
	void NotifyStart();

private:
	optional_ptr<ClientContext> context;
	vector<shared_ptr<ClientContextState>> listeners;
	TaskInfo info;
};

} // namespace duckdb
