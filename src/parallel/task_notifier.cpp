#include "duckdb/parallel/task_notifier.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"

namespace duckdb {

TaskNotifier::TaskNotifier(optional_ptr<ClientContext> context_p) : context(context_p) {
	if (!context || !context->registered_state->HasTaskListeners()) {
		return;
	}
	listeners = context->registered_state->TaskListeners();
	for (auto &state : listeners) {
		state->OnTaskStart(*context);
	}
}

TaskNotifier::~TaskNotifier() {
	for (auto &state : listeners) {
		state->OnTaskStop(*context);
	}
}

} // namespace duckdb
