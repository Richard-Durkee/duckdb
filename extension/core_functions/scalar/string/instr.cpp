#include "core_functions/scalar/string_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/function/scalar/string_common.hpp"

namespace duckdb {

struct InstrOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA haystack, TB needle) {
		auto location = FindStrInStr(haystack, needle);
		if (location == DConstants::INVALID_INDEX) {
			return 0;
		}
		D_ASSERT(location <= haystack.GetSize());
		// the position is one past the number of codepoints before the match
		string_t prefix(haystack.GetData(), UnsafeNumericCast<uint32_t>(location));
		return Length<string_t, TR>(prefix) + 1;
	}
};

struct InstrAsciiOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA haystack, TB needle) {
		auto location = FindStrInStr(haystack, needle);
		return UnsafeNumericCast<TR>(location == DConstants::INVALID_INDEX ? 0U : location + 1U);
	}
};

static unique_ptr<BaseStatistics> InStrPropagateStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	D_ASSERT(child_stats.size() == 2);
	// can only propagate stats if the children have stats
	// for strpos, we only care if the FIRST string has unicode or not
	if (!StringStats::CanContainUnicode(child_stats[0])) {
		expr.FunctionMutable().SetFunctionCallback(
		    ScalarFunction::BinaryFunction<string_t, string_t, int64_t, InstrAsciiOperator>);
	}
	return nullptr;
}

ScalarFunction InstrFun::GetFunction() {
	auto function = ScalarFunction({}, LogicalType::BIGINT,
	                               ScalarFunction::BinaryFunction<string_t, string_t, int64_t, InstrOperator>, nullptr,
	                               InStrPropagateStats);
	function.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("search_string", LogicalType::VARCHAR);
	function.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	return function;
}

} // namespace duckdb
