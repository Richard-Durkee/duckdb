//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/query_context.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {

class ClientContext;

//! The QueryContext wraps an optional client context.
//! It makes query-related information available to operations.
class MemoryAccount;

class QueryContext {
public:
	QueryContext() : context(nullptr) {
	}
	QueryContext(optional_ptr<ClientContext> context) : context(context) { // NOLINT: allow implicit construction
	}
	QueryContext(ClientContext &context) : context(&context) { // NOLINT: allow implicit construction
	}
	QueryContext(optional_ptr<ClientContext> context, optional_ptr<MemoryAccount> memory_account)
	    : context(context), memory_account(memory_account) {
	}

public:
	bool Valid() const {
		return context != nullptr;
	}
	optional_ptr<ClientContext> GetClientContext() const {
		return context;
	}
	//! The account buffer-managed memory allocated with this context is charged to, if any
	optional_ptr<MemoryAccount> GetMemoryAccount() const {
		return memory_account;
	}

private:
	optional_ptr<ClientContext> context;
	optional_ptr<MemoryAccount> memory_account;
};

} // namespace duckdb
