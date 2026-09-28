#include "duckdb/parallel/task_notifier.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/executor_task.hpp"

namespace duckdb {

TaskNotifier::TaskNotifier(optional_ptr<ClientContext> context_p, const Task &task) : context(context_p), info(task) {
	if (CaptureListeners()) {
		NotifyStart();
	}
}

TaskNotifier::TaskNotifier(ClientContext &context_p, const ExecutorTask &task) : context(context_p), info(task) {
	if (CaptureListeners()) {
		info.pipeline = task.GetPipeline();
		info.op = task.op;
		NotifyStart();
	}
}

TaskNotifier::~TaskNotifier() {
	for (auto &state : listeners) {
		state->OnTaskStop(*context, info);
	}
}

bool TaskNotifier::CaptureListeners() {
	if (!context || !context->registered_state->HasTaskListeners()) {
		return false;
	}
	listeners = context->registered_state->TaskListeners();
	return !listeners.empty();
}

void TaskNotifier::NotifyStart() {
	for (auto &state : listeners) {
		state->OnTaskStart(*context, info);
	}
}

} // namespace duckdb
