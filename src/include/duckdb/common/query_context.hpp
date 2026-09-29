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
struct OperatorMemoryCounter;

class QueryContext {
public:
	QueryContext() : context(nullptr) {
	}
	QueryContext(optional_ptr<ClientContext> context) : context(context) { // NOLINT: allow implicit construction
	}
	QueryContext(ClientContext &context) : context(&context) { // NOLINT: allow implicit construction
	}
	//! POC: a context that also names the operator that owns memory allocated with it
	QueryContext(ClientContext &context, optional_ptr<OperatorMemoryCounter> memory_owner)
	    : context(&context), memory_owner(memory_owner) {
	}

public:
	bool Valid() const {
		return context != nullptr;
	}
	optional_ptr<ClientContext> GetClientContext() const {
		return context;
	}
	optional_ptr<OperatorMemoryCounter> GetMemoryOwner() const {
		return memory_owner;
	}

private:
	optional_ptr<ClientContext> context;
	optional_ptr<OperatorMemoryCounter> memory_owner;
};

} // namespace duckdb
