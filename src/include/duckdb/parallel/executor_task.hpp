//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/executor_task.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parallel/task.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {
class Event;
class PhysicalOperator;
class ThreadContext;
struct OperatorMemoryCounter;

//! Execute a task within an executor, including exception handling
//! This should be used within queries
class ExecutorTask : public Task {
public:
	ExecutorTask(Executor &executor, shared_ptr<Event> event);
	ExecutorTask(ClientContext &context, shared_ptr<Event> event, const PhysicalOperator &op);
	~ExecutorTask() override;

public:
	void Deschedule() override;
	void Reschedule() override;

public:
	Executor &executor;
	shared_ptr<Event> event;
	unique_ptr<ThreadContext> thread_context;
	optional_ptr<const PhysicalOperator> op;

private:
	ClientContext &context;
	//! PROTOTYPE: memory counter of `op`, current while the task runs; nullptr when profiling is disabled
	shared_ptr<OperatorMemoryCounter> memory_counter;
	//! PROTOTYPE: the query's unattributed-memory counter, current underneath any operator scope while the task runs
	shared_ptr<OperatorMemoryCounter> query_memory_counter;

public:
	virtual TaskExecutionResult ExecuteTask(TaskExecutionMode mode) = 0;
	TaskExecutionResult Execute(TaskExecutionMode mode) override;
};

} // namespace duckdb
