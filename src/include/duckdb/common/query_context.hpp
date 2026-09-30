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
class MemoryTracker;

class QueryContext {
public:
	QueryContext() : context(nullptr) {
	}
	QueryContext(optional_ptr<ClientContext> context) : context(context) { // NOLINT: allow implicit construction
	}
	QueryContext(ClientContext &context) : context(&context) { // NOLINT: allow implicit construction
	}
	QueryContext(optional_ptr<ClientContext> context, optional_ptr<MemoryTracker> memory_tracker)
	    : context(context), memory_tracker(memory_tracker) {
	}

public:
	bool Valid() const {
		return context != nullptr;
	}
	optional_ptr<ClientContext> GetClientContext() const {
		return context;
	}
	//! The tracker buffer-managed memory allocated with this context is charged to, if any
	optional_ptr<MemoryTracker> GetMemoryTracker() const {
		return memory_tracker;
	}

private:
	optional_ptr<ClientContext> context;
	optional_ptr<MemoryTracker> memory_tracker;
};

} // namespace duckdb
