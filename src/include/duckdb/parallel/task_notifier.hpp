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

namespace duckdb {
class ClientContext;
class ClientContextState;

//! The TaskNotifier notifies ClientContextState listeners about started / stopped tasks
//! The listeners are captured on start, so every state that sees OnTaskStart also sees OnTaskStop
class TaskNotifier {
public:
	explicit TaskNotifier(optional_ptr<ClientContext> context_p);

	~TaskNotifier();

private:
	optional_ptr<ClientContext> context;
	vector<shared_ptr<ClientContextState>> listeners;
};

} // namespace duckdb
