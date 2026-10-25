#include "duckdb/common/exception.hpp"
#include "duckdb/common/swar.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/scalar/list_functions.hpp"
#include "duckdb/function/scalar/map_functions.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/function/scalar/struct_functions.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

namespace duckdb {

namespace {

//! Loads eight bytes so that the first byte in memory is the least significant byte
static inline uint64_t LoadLittleEndian(const unsigned char *ptr) {
#if DUCKDB_IS_BIG_ENDIAN
	return BSwap(Load<uint64_t>(ptr));
#else
	return Load<uint64_t>(ptr);
#endif
}

//! Linear-time (Knuth-Morris-Pratt) search, used when the needle keeps almost-matching the haystack
static idx_t ContainsLinear(const unsigned char *haystack, idx_t haystack_size, const unsigned char *needle,
                            idx_t needle_size, idx_t base_offset) {
	// prefix[i] is the length of the longest proper prefix of needle[0..i] that is also a suffix of it
	vector<idx_t> prefix(needle_size, 0);
	for (idx_t i = 1, k = 0; i < needle_size; i++) {
		while (k > 0 && needle[i] != needle[k]) {
			k = prefix[k - 1];
		}
		if (needle[i] == needle[k]) {
			k++;
		}
		prefix[i] = k;
	}
	for (idx_t i = 0, k = 0; i < haystack_size; i++) {
		while (k > 0 && haystack[i] != needle[k]) {
			k = prefix[k - 1];
		}
		if (haystack[i] == needle[k]) {
			k++;
		}
		if (k == needle_size) {
			return base_offset + i + 1 - needle_size;
		}
	}
	return DConstants::INVALID_INDEX;
}

//! Finds the needle by checking eight candidate positions at a time: a candidate needs to match both the first and the
//! last byte of the needle before the whole needle is compared
static idx_t ContainsSwar(const unsigned char *haystack, idx_t haystack_size, const unsigned char *needle,
                          idx_t needle_size) {
	D_ASSERT(needle_size >= 2);
	if (needle_size > haystack_size) {
		return DConstants::INVALID_INDEX;
	}
	const idx_t candidate_end = haystack_size - (needle_size - 1);
	const auto first = SwarWord::Repeat(needle[0]);
	const auto last = SwarWord::Repeat(needle[needle_size - 1]);
	// the number of candidates whose remainder we compare before switching, roughly linear in the haystack
	const idx_t max_full_compares = 16 + (haystack_size * 8) / needle_size;
	idx_t full_compares = 0;
	idx_t offset = 0;
	for (; offset + SwarWord::SIZE <= candidate_end; offset += SwarWord::SIZE) {
		auto candidates = SwarWord::MaybeZeroBytes(LoadLittleEndian(haystack + offset) ^ first) &
		                  SwarWord::MaybeZeroBytes(LoadLittleEndian(haystack + offset + needle_size - 1) ^ last);
		while (candidates) {
			const auto position = offset + CountZeros<uint64_t>::Trailing(candidates) / 8;
			if (memcmp(haystack + position, needle, needle_size) == 0) {
				return position;
			}
			if (++full_compares > max_full_compares) {
				return ContainsLinear(haystack + position, haystack_size - position, needle, needle_size, position);
			}
			candidates &= candidates - 1;
		}
	}
	for (; offset < candidate_end; offset++) {
		if (haystack[offset] == needle[0] && memcmp(haystack + offset, needle, needle_size) == 0) {
			return offset;
		}
	}
	return DConstants::INVALID_INDEX;
}

struct ContainsOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		return FindStrInStr(left, right) != DConstants::INVALID_INDEX;
	}
};

} // namespace

idx_t FindStrInStr(const unsigned char *haystack, idx_t haystack_size, const unsigned char *needle, idx_t needle_size) {
	D_ASSERT(needle_size > 0);
	if (needle_size == 1) {
		auto location = memchr(haystack, needle[0], haystack_size);
		if (location == nullptr) {
			return DConstants::INVALID_INDEX;
		}
		return UnsafeNumericCast<idx_t>(const_uchar_ptr_cast(location) - haystack);
	}
	return ContainsSwar(haystack, haystack_size, needle, needle_size);
}

idx_t FindStrInStr(const string_t &haystack_s, const string_t &needle_s) {
	auto haystack = const_uchar_ptr_cast(haystack_s.GetData());
	auto haystack_size = haystack_s.GetSize();
	auto needle = const_uchar_ptr_cast(needle_s.GetData());
	auto needle_size = needle_s.GetSize();
	if (needle_size == 0) {
		// empty needle: always true
		return 0;
	}
	return FindStrInStr(haystack, haystack_size, needle, needle_size);
}

static FilterPropagateResult ContainsFilterPrune(const FunctionStatisticsPruneInput &input) {
	auto &children = input.function.GetChildren();
	if (children.size() != 2 || children[1]->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}

	auto haystack_stats = input.ChildStats(0);
	if (!haystack_stats || haystack_stats->GetStatsType() != StatisticsType::STRING_STATS) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (!haystack_stats->CanHaveNoNull()) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}

	auto &needle_value = children[1]->Cast<BoundConstantExpression>().GetValue();
	if (needle_value.IsNull()) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	auto &needle = StringValue::Get(needle_value);

	// if the max string length is less than the needle size, we can prune the filter.
	if (StringStats::HasMaxStringLength(*haystack_stats) &&
	    StringStats::MaxStringLength(*haystack_stats) < needle.size()) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}

	if (!StringStats::HasMinMax(*haystack_stats)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}

	const auto min = StringStats::Min(*haystack_stats);
	const auto max = StringStats::Max(*haystack_stats);
	if (min != max) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	// All strings share the stored prefix, so a needle within it always matches.
	if (FindStrInStr(string_t(min), string_t(needle)) == DConstants::INVALID_INDEX) {
		// A missing needle could still occur beyond the truncated prefix.
		if (StringStats::GetMinType(*haystack_stats) != StringStatsType::EXACT_STATS ||
		    StringStats::GetMaxType(*haystack_stats) != StringStatsType::EXACT_STATS) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	return haystack_stats->CanHaveNull() ? FilterPropagateResult::FILTER_TRUE_OR_NULL
	                                     : FilterPropagateResult::FILTER_ALWAYS_TRUE;
}

ScalarFunction GetStringContains() {
	ScalarFunction string_fun("contains", {}, LogicalType::BOOLEAN,
	                          ScalarFunction::BinaryFunction<string_t, string_t, bool, ContainsOperator>);
	string_fun.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("search_string", LogicalType::VARCHAR);
	string_fun.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	string_fun.SetFilterPruneCallback(ContainsFilterPrune);
	return string_fun;
}

ScalarFunctionSet ContainsFun::GetFunctions() {
	auto string_fun = GetStringContains();
	auto list_fun = ListContainsFun::GetFunction();
	auto map_fun = MapContainsFun::GetFunction();
	auto struct_fun = StructContainsFun::GetFunction();
	ScalarFunctionSet set("contains");
	set.AddFunction(string_fun);
	set.AddFunction(list_fun);
	set.AddFunction(map_fun);
	set.AddFunction(struct_fun);
	return set;
}

} // namespace duckdb
