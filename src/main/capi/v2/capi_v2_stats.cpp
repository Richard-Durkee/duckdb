#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/main/capi_v2/capi_v2_function_internal.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

namespace duckdb::capiv2 {

BaseStatistics &CV2Stats::GetMutableStats(const char *function_name) {
	if (!mutable_stats) {
		throw InvalidInputException("%s: the statistics are read-only", function_name);
	}
	return *mutable_stats;
}

//! Whether the statistics carry a minimum and a maximum
static bool CV2StatsHasRange(const BaseStatistics &stats) {
	switch (stats.GetStatsType()) {
	case StatisticsType::NUMERIC_STATS:
	case StatisticsType::STRING_STATS:
		return true;
	default:
		return false;
	}
}

//! The minimum or maximum of the statistics, as a value of the type they describe; NULL when it is unknown
static Value CV2StatsGetBound(const BaseStatistics &stats, bool is_min) {
	const auto &type = stats.GetType();
	switch (stats.GetStatsType()) {
	case StatisticsType::NUMERIC_STATS:
		if (is_min) {
			return NumericStats::HasMin(stats) ? NumericStats::Min(stats) : Value(type);
		}
		return NumericStats::HasMax(stats) ? NumericStats::Max(stats) : Value(type);
	case StatisticsType::STRING_STATS: {
		if (is_min ? !StringStats::HasMin(stats) : !StringStats::HasMax(stats)) {
			return Value(type);
		}
		// a truncated bound is widened to the nearest string that still bounds every value
		auto bound = is_min ? StringStats::TryGetValidMin(stats) : StringStats::TryGetValidMax(stats);
		if (bound.IsNull()) {
			return Value(type);
		}
		if (type.id() == LogicalTypeId::BLOB) {
			return Value::BLOB_RAW(StringValue::Get(bound));
		}
		return bound;
	}
	default:
		return Value(type);
	}
}

static void CV2StatsSetBound(BaseStatistics &stats, const Value &input, bool is_min, const char *function_name) {
	const auto &type = stats.GetType();
	if (!CV2StatsHasRange(stats)) {
		throw InvalidInputException("%s: statistics of type %s have no range", function_name, type.ToString());
	}
	const bool is_string = stats.GetStatsType() == StatisticsType::STRING_STATS;
	if (input.IsNull()) {
		if (is_string) {
			auto empty = string_t();
			is_min ? StringStats::SetMin(stats, empty, StringStatsType::NO_STATS)
			       : StringStats::SetMax(stats, empty, StringStatsType::NO_STATS);
		} else {
			is_min ? NumericStats::SetMin(stats, Value(type)) : NumericStats::SetMax(stats, Value(type));
		}
		return;
	}

	string error;
	auto cast = input.DefaultTryCastAs(type, &error, true);
	if (!cast) {
		throw InvalidInputException("%s: the value %s can not be cast to %s: %s", function_name, input.ToString(),
		                            type.ToString(), error);
	}
	auto bound = std::move(*cast);
	auto other = CV2StatsGetBound(stats, !is_min);
	if (!other.IsNull() && (is_min ? bound > other : bound < other)) {
		throw InvalidInputException("%s: the %s %s is %s the %s %s", function_name, is_min ? "minimum" : "maximum",
		                            bound.ToString(), is_min ? "greater than" : "less than",
		                            is_min ? "maximum" : "minimum", other.ToString());
	}

	if (!is_string) {
		is_min ? NumericStats::SetMin(stats, bound) : NumericStats::SetMax(stats, bound);
		return;
	}
	const auto &str = StringValue::Get(bound);
	if (str.size() > StringStatsData::MAXIMUM_MIN_STRING_LENGTH) {
		throw InvalidInputException("%s: the %s is longer than the %llu bytes string statistics can hold",
		                            function_name, is_min ? "minimum" : "maximum",
		                            static_cast<uint64_t>(StringStatsData::MAXIMUM_MIN_STRING_LENGTH));
	}
	string_t value(str.c_str(), UnsafeNumericCast<uint32_t>(str.size()));
	is_min ? StringStats::SetMin(stats, value, StringStatsType::EXACT_STATS)
	       : StringStats::SetMax(stats, value, StringStatsType::EXACT_STATS);
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_stats_can_have_null(duckdb_v2_stats_handle stats, bool *out,
                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = Convert(stats)->GetStats().CanHaveNull(); });
}

DUCKDB_V2_ERROR duckdb_v2_stats_can_have_valid(duckdb_v2_stats_handle stats, bool *out,
                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = Convert(stats)->GetStats().CanHaveNoNull(); });
}

DUCKDB_V2_ERROR duckdb_v2_stats_set_can_have_null(duckdb_v2_stats_handle stats, bool value,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_set_can_have_null");
		result.Set(value ? duckdb::StatsInfo::CAN_HAVE_NULL_VALUES : duckdb::StatsInfo::CANNOT_HAVE_NULL_VALUES);
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_set_can_have_valid(duckdb_v2_stats_handle stats, bool value,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_set_can_have_valid");
		result.Set(value ? duckdb::StatsInfo::CAN_HAVE_VALID_VALUES : duckdb::StatsInfo::CANNOT_HAVE_VALID_VALUES);
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_get_min(duckdb_v2_stats_handle stats, duckdb_v2_value_handle *value,
                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(value);
	*value = nullptr;
	return WithErrorHandler(
	    err, [&]() { *value = Convert(new duckdb::Value(CV2StatsGetBound(Convert(stats)->GetStats(), true))); });
}

DUCKDB_V2_ERROR duckdb_v2_stats_get_max(duckdb_v2_stats_handle stats, duckdb_v2_value_handle *value,
                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(value);
	*value = nullptr;
	return WithErrorHandler(
	    err, [&]() { *value = Convert(new duckdb::Value(CV2StatsGetBound(Convert(stats)->GetStats(), false))); });
}

DUCKDB_V2_ERROR duckdb_v2_stats_set_min(duckdb_v2_stats_handle stats, duckdb_v2_value_handle value,
                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_set_min");
		CV2StatsSetBound(result, *Convert(value), true, "duckdb_v2_stats_set_min");
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_set_max(duckdb_v2_stats_handle stats, duckdb_v2_value_handle value,
                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_set_max");
		CV2StatsSetBound(result, *Convert(value), false, "duckdb_v2_stats_set_max");
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_get_distinct_count(duckdb_v2_stats_handle stats, uint64_t *out,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() {
		// GetDistinctCount is not const, though it only reads
		*out = const_cast<duckdb::BaseStatistics &>(Convert(stats)->GetStats()).GetDistinctCount();
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_set_distinct_count(duckdb_v2_stats_handle stats, uint64_t value,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_set_distinct_count");
		result.SetDistinctCount(value);
	});
}
