//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/client_context_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/enums/prepared_statement_mode.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/main/valid_checker.hpp"
#include "duckdb/planner/expression/bound_parameter_data.hpp"
#include <mutex>

namespace duckdb {
class ClientContext;
class ErrorData;
class MetaTransaction;
class PreparedStatementData;
class SQLStatement;
struct PendingQueryParameters;
class RegisteredStateManager;

enum class RebindQueryInfo { DO_NOT_REBIND, ATTEMPT_TO_REBIND };

struct BindPreparedStatementCallbackInfo {
	PreparedStatementData &prepared_statement;
	optional_ptr<identifier_map_t<BoundParameterData>> parameters;
};

//! ClientContextState is virtual base class for ClientContext-local (or Query-Local, using QueryEnd callback) state
//! e.g. caches that need to live as long as a ClientContext or Query.
class ClientContextState {
public:
	virtual ~ClientContextState() = default;
	virtual void QueryBegin(ClientContext &context) {
	}
	virtual void QueryEnd() {
	}
	virtual void QueryEnd(ClientContext &context) {
		QueryEnd();
	}
	virtual void QueryEnd(ClientContext &context, optional_ptr<ErrorData> error) {
		QueryEnd(context);
	}
	virtual void TransactionBegin(MetaTransaction &transaction, ClientContext &context) {
	}
	virtual void TransactionCommit(MetaTransaction &transaction, ClientContext &context) {
	}
	virtual void TransactionRollback(MetaTransaction &transaction, ClientContext &context) {
	}
	virtual void TransactionRollback(MetaTransaction &transaction, ClientContext &context,
	                                 optional_ptr<ErrorData> error) {
		TransactionRollback(transaction, context);
	}
	virtual bool CanRequestRebind() {
		return false;
	}
	virtual RebindQueryInfo OnPlanningError(ClientContext &context, SQLStatement &statement, ErrorData &error) {
		return RebindQueryInfo::DO_NOT_REBIND;
	}
	virtual RebindQueryInfo OnFinalizePrepare(ClientContext &context, PreparedStatementData &prepared_statement,
	                                          PreparedStatementMode mode) {
		return RebindQueryInfo::DO_NOT_REBIND;
	}
	virtual RebindQueryInfo OnRebindPreparedStatement(ClientContext &context, BindPreparedStatementCallbackInfo &info,
	                                                  RebindQueryInfo current_rebind) {
		return RebindQueryInfo::DO_NOT_REBIND;
	}
	virtual void WriteProfilingInformation(std::ostream &ss) {
	}
	//! Whether OnTaskStart / OnTaskStop are invoked for this state - queried once when the state is registered
	//! Task callbacks fire for every executed task, so states that do not use them should return false
	virtual bool ReceivesTaskCallbacks() const {
		return true;
	}
	virtual void OnTaskStart(ClientContext &context) {
	}
	virtual void OnTaskStop(ClientContext &context) {
	}

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

class RegisteredStateManager {
public:
	template <class T, typename... ARGS>
	shared_ptr<T> GetOrCreate(const string &key, ARGS &&... args) {
		lock_guard<mutex> l(lock);
		auto lookup = registered_state.find(key);
		if (lookup != registered_state.end()) {
			return shared_ptr_cast<ClientContextState, T>(lookup->second);
		}
		auto cache = make_shared_ptr<T>(std::forward<ARGS>(args)...);
		registered_state[key] = cache;
		UpdateTaskListeners();
		return cache;
	}

	template <class T>
	shared_ptr<T> Get(const string &key) {
		lock_guard<mutex> l(lock);
		auto lookup = registered_state.find(key);
		if (lookup == registered_state.end()) {
			return nullptr;
		}
		return shared_ptr_cast<ClientContextState, T>(lookup->second);
	}

	void Insert(const string &key, shared_ptr<ClientContextState> state_p) {
		lock_guard<mutex> l(lock);
		if (registered_state.insert(make_pair(key, std::move(state_p))).second) {
			UpdateTaskListeners();
		}
	}

	void Remove(const string &key) {
		lock_guard<mutex> l(lock);
		if (registered_state.erase(key) > 0) {
			UpdateTaskListeners();
		}
	}

	vector<shared_ptr<ClientContextState>> States() {
		lock_guard<mutex> l(lock);
		vector<shared_ptr<ClientContextState>> states;
		for (auto &entry : registered_state) {
			states.push_back(entry.second);
		}
		return states;
	}

	//! Whether any registered state receives task callbacks - cheap enough to check for every task
	bool HasTaskListeners() const {
		return has_task_listeners.load(std::memory_order_relaxed);
	}

	//! The registered states that receive task callbacks
	vector<shared_ptr<ClientContextState>> TaskListeners() {
		lock_guard<mutex> l(lock);
		return task_listeners;
	}

private:
	void UpdateTaskListeners() {
		task_listeners.clear();
		for (auto &entry : registered_state) {
			if (entry.second->ReceivesTaskCallbacks()) {
				task_listeners.push_back(entry.second);
			}
		}
		has_task_listeners.store(!task_listeners.empty(), std::memory_order_relaxed);
	}

private:
	mutex lock;
	unordered_map<string, shared_ptr<ClientContextState>> registered_state;
	//! The subset of registered_state that receives task callbacks
	vector<shared_ptr<ClientContextState>> task_listeners;
	//! Only written on registration, so reading it does not contend across threads
	atomic<bool> has_task_listeners {false};
};

} // namespace duckdb
