//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector_operations/general_cast.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

struct VectorTryCastData {
	VectorTryCastData(Vector &result_p, CastParameters &parameters) : result(result_p), parameters(parameters) {
	}

	Vector &result;
	CastParameters &parameters;
	bool all_converted = true;
};

struct HandleVectorCastError {
	//! Whether a failing row needs to produce an error message: a try cast only keeps the first one
	static bool NeedsErrorMessage(const VectorTryCastData &cast_data) {
		auto error_message = cast_data.parameters.error_message;
		return !error_message || error_message->empty();
	}

	template <class RESULT_TYPE, class MASK_TYPE>
	static RESULT_TYPE Operation(const string &error_message, MASK_TYPE &mask, idx_t idx,
	                             VectorTryCastData &cast_data) {
		if (NeedsErrorMessage(cast_data)) {
			HandleCastError::AssignError(error_message, cast_data.parameters);
		}
		cast_data.all_converted = false;
		mask.SetInvalid(idx);
		return NullValue<RESULT_TYPE>();
	}
};

} // namespace duckdb
