#include "test_capi_v2.hpp"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V2 table filter tests: filters pushed into the scan of a table function that enabled filter pushdown.
//
// c_filtered(n) produces one BIGINT column "v" carrying 0..n-1. Its global init callback records what it can read
// about each pushed filter - including what the filter evaluates to over a few ranges - into file-scope statics that
// the tests assert on after the query. Callbacks avoid Catch assertions, which would throw through the C boundary.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

struct FilterObservations {
	idx_t filter_count = 0;
	idx_t local_filter_count = 0;
	std::vector<idx_t> columns;
	std::vector<bool> optional;
	std::vector<DUCKDB_V2_EXPRESSION_TYPE> types;
	//! What each filter evaluates to over [0, 5], [6, 9], an unknown range, and an entirely NULL block
	std::vector<std::vector<DUCKDB_V2_FILTER_PROPAGATE_RESULT>> ranges;
	bool out_of_bounds_failed = false;
	std::string failure;

	void Reset() {
		*this = FilterObservations();
	}
};

FilterObservations filter_observed;

struct FilteredBind {
	int64_t count = 0;
	bool claim_applied = false;
};
struct FilteredGlobal {
	int64_t position = 0;
};

void DeleteFilteredBind(void *ptr) {
	delete static_cast<FilteredBind *>(ptr);
}
void DeleteFilteredGlobal(void *ptr) {
	delete static_cast<FilteredGlobal *>(ptr);
}

void Fail(std::string_view what) {
	if (filter_observed.failure.empty()) {
		filter_observed.failure = what;
	}
}

void FilteredBindCb(duckdb_v2_function_bind_info_handle info, duckdb_v2_table_function_bind_info_handle result,
                    duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_function_bind_get_arg_value(info, 0, &value, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	int64_t count = 0;
	auto rc = duckdb_v2_value_get_bigint(value, &count, err);
	duckdb_v2_value_destroy(&value);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	void *user_data = nullptr;
	duckdb_v2_function_bind_get_user_data(info, &user_data, nullptr);

	duckdb_v2_logical_type_handle bigint = nullptr;
	if (duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, nullptr, nullptr, nullptr, 0,
	                                          &bigint, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	std::string_view name = "v";
	rc = duckdb_v2_table_function_bind_add_result_column(result, name.data(), name.size(), bigint, err);
	duckdb_v2_logical_type_destroy(&bigint);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_opaque bind_data = {new FilteredBind {count, user_data != nullptr}, DeleteFilteredBind, nullptr};
	duckdb_v2_function_bind_set_bind_data(info, &bind_data, err);
}

DUCKDB_V2_FILTER_PROPAGATE_RESULT CheckRange(duckdb_v2_factory_handle factory, duckdb_v2_table_filter_handle filter,
                                             int64_t min, int64_t max, bool known, bool can_have_valid) {
	duckdb_v2_value_handle min_value = nullptr;
	duckdb_v2_value_handle max_value = nullptr;
	if (known) {
		duckdb_v2_value_create_bigint(factory, min, &min_value, nullptr);
		duckdb_v2_value_create_bigint(factory, max, &max_value, nullptr);
	}
	auto result = DUCKDB_V2_FILTER_PROPAGATE_RESULT_NO_PRUNING_POSSIBLE;
	if (duckdb_v2_table_filter_check_range(filter, min_value, max_value, false, can_have_valid, &result, nullptr) !=
	    DUCKDB_V2_ERROR_NONE) {
		Fail("check_range");
	}
	duckdb_v2_value_destroy(&min_value);
	duckdb_v2_value_destroy(&max_value);
	return result;
}

void FilteredInitGlobalCb(duckdb_v2_table_function_init_global_info_handle info, duckdb_v2_context_handle context,
                          duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	idx_t count = 0;
	if (duckdb_v2_table_function_init_global_get_filter_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	filter_observed.filter_count = count;
	for (idx_t i = 0; i < count; i++) {
		duckdb_v2_table_filter_handle filter = nullptr;
		idx_t column = 0;
		bool optional = false;
		duckdb_v2_expression_handle expression = nullptr;
		auto type = DUCKDB_V2_EXPRESSION_TYPE_INVALID;
		if (duckdb_v2_table_function_init_global_get_filter(info, i, &filter, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_table_filter_get_column_index(filter, &column, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_table_filter_is_optional(filter, &optional, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_table_filter_get_expression(filter, &expression, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_expression_get_type(expression, &type, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		filter_observed.columns.push_back(column);
		filter_observed.optional.push_back(optional);
		filter_observed.types.push_back(type);
		filter_observed.ranges.push_back(
		    {CheckRange(factory, filter, 0, 5, true, true), CheckRange(factory, filter, 6, 9, true, true),
		     CheckRange(factory, filter, 0, 0, false, true), CheckRange(factory, filter, 0, 0, false, false)});
	}
	duckdb_v2_table_filter_handle missing = nullptr;
	duckdb_v2_error_info_handle oob = nullptr;
	filter_observed.out_of_bounds_failed =
	    duckdb_v2_table_function_init_global_get_filter(info, count, &missing, &oob) != DUCKDB_V2_ERROR_NONE &&
	    missing == nullptr;
	duckdb_v2_error_info_destroy(&oob);

	duckdb_v2_opaque state = {new FilteredGlobal {0}, DeleteFilteredGlobal, nullptr};
	duckdb_v2_table_function_init_global_set_global_state(info, &state, err);
}

void FilteredInitLocalCb(duckdb_v2_table_function_init_local_info_handle info, duckdb_v2_context_handle,
                         duckdb_v2_error_info_handle *err) {
	duckdb_v2_table_function_init_local_get_filter_count(info, &filter_observed.local_filter_count, err);
}

void FilteredExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle,
                    duckdb_v2_error_info_handle *err) {
	void *bind_ptr = nullptr;
	void *global_ptr = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	if (duckdb_v2_table_function_exec_get_bind_data(info, &bind_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &bind = *static_cast<FilteredBind *>(bind_ptr);
	auto &global = *static_cast<FilteredGlobal *>(global_ptr);
	duckdb_v2_vector_handle vec = nullptr;
	void *raw = nullptr;
	if (duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(vec, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	// two rows per batch, so that most batches of a selective query are filtered down to no rows
	auto produced = bind.count - global.position < 2 ? bind.count - global.position : 2;
	for (int64_t i = 0; i < produced; i++) {
		static_cast<int64_t *>(raw)[i] = global.position + i;
	}
	global.position += produced;
	if (duckdb_v2_vector_set_size(vec, static_cast<idx_t>(produced), err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	if (bind.claim_applied) {
		duckdb_v2_table_function_exec_set_filters_applied(info, true, err);
	}
}

int dummy_user_data = 0;

duckdb_v2_table_function_handle MakeFiltered(EnvFixture &fx, std::string_view name, bool projection_pushdown,
                                             bool claim_applied) {
	duckdb_v2_table_function_handle function = nullptr;
	REQUIRE(duckdb_v2_table_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_name(function, name.data(), name.size(), nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_table_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto bigint = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	std::string_view param = "n";
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, param.data(), param.size(), bigint, nullptr,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&bigint);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, FilteredBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_global_callback(function, FilteredInitGlobalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_local_callback(function, FilteredInitLocalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, FilteredExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_projection_pushdown(function, projection_pushdown, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_filter_pushdown(function, true, nullptr) == DUCKDB_V2_ERROR_NONE);
	if (claim_applied) {
		duckdb_v2_opaque user_data = {&dummy_user_data, nullptr, nullptr};
		REQUIRE(duckdb_v2_table_function_set_user_data(function, &user_data, nullptr) == DUCKDB_V2_ERROR_NONE);
	}
	return function;
}

void RegisterFiltered(EnvFixture &fx, std::string_view name, bool claim_applied = false) {
	auto function = MakeFiltered(fx, name, true, claim_applied);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
}

int64_t QueryBigint(duckdb_v2_connection_handle conn, std::string_view sql) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(result);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	auto out = static_cast<const int64_t *>(view.data)[SelAt(view.sel, 0)];
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&result);
	return out;
}

} // namespace

TEST_CASE("C-API-V2: table function filter pushdown hands the scan its filters", "[capi_v2]") {
	EnvFixture fx;
	RegisterFiltered(fx, "c_filtered");

	filter_observed.Reset();
	REQUIRE(QueryBigint(fx.conn, "SELECT count(*) FROM c_filtered(10) WHERE v >= 7") == 3);
	REQUIRE(filter_observed.failure.empty());
	REQUIRE(filter_observed.filter_count == 1);
	REQUIRE(filter_observed.local_filter_count == 1);
	REQUIRE(filter_observed.columns == std::vector<idx_t> {0});
	REQUIRE(filter_observed.optional == std::vector<bool> {false});
	REQUIRE(filter_observed.types ==
	        std::vector<DUCKDB_V2_EXPRESSION_TYPE> {DUCKDB_V2_EXPRESSION_TYPE_COMPARE_GREATERTHANOREQUALTO});
	REQUIRE(filter_observed.out_of_bounds_failed);
	// v >= 7 over [0, 5] is false, over [6, 9] unknown, over an unknown range unknown, over NULLs false or NULL
	REQUIRE(filter_observed.ranges.size() == 1);
	auto &ranges = filter_observed.ranges[0];
	REQUIRE(ranges[0] == DUCKDB_V2_FILTER_PROPAGATE_RESULT_FILTER_ALWAYS_FALSE);
	REQUIRE(ranges[1] == DUCKDB_V2_FILTER_PROPAGATE_RESULT_NO_PRUNING_POSSIBLE);
	REQUIRE(ranges[2] == DUCKDB_V2_FILTER_PROPAGATE_RESULT_NO_PRUNING_POSSIBLE);
	REQUIRE((ranges[3] == DUCKDB_V2_FILTER_PROPAGATE_RESULT_FILTER_ALWAYS_FALSE ||
	         ranges[3] == DUCKDB_V2_FILTER_PROPAGATE_RESULT_FILTER_FALSE_OR_NULL));

	// v = 4 is false over [6, 9]
	filter_observed.Reset();
	REQUIRE(QueryBigint(fx.conn, "SELECT count(*) FROM c_filtered(10) WHERE v = 4") == 1);
	REQUIRE(filter_observed.filter_count == 1);
	REQUIRE(filter_observed.ranges[0][1] == DUCKDB_V2_FILTER_PROPAGATE_RESULT_FILTER_ALWAYS_FALSE);

	// without a filter on its columns the scan sees none
	filter_observed.Reset();
	REQUIRE(QueryBigint(fx.conn, "SELECT count(*) FROM c_filtered(10)") == 10);
	REQUIRE(filter_observed.filter_count == 0);
	REQUIRE(filter_observed.out_of_bounds_failed);
}

TEST_CASE("C-API-V2: the engine applies the filters pushed into a table function", "[capi_v2]") {
	EnvFixture fx;
	RegisterFiltered(fx, "c_filtered");
	RegisterFiltered(fx, "c_filtered_claimed", true);

	// the scan never filters: the engine does, also when every row of most batches is rejected
	REQUIRE(QueryBigint(fx.conn, "SELECT count(*) FROM c_filtered(1000) WHERE v = 999") == 1);
	REQUIRE(QueryBigint(fx.conn, "SELECT sum(v) FROM c_filtered(1000) WHERE v IN (1, 500, 999, 4000)") == 1500);
	REQUIRE(QueryBigint(fx.conn, "SELECT count(*) FROM c_filtered(1000) WHERE v > 10 AND v < 20") == 9);
	REQUIRE(QueryBigint(fx.conn, "SELECT count(*) FROM c_filtered(1000) WHERE v IS NOT NULL") == 1000);
	REQUIRE(QueryBigint(fx.conn, "SELECT count(*) FROM c_filtered(1000) WHERE v < 0") == 0);
	// a scan that claims to have applied the filters is taken at its word
	REQUIRE(QueryBigint(fx.conn, "SELECT count(*) FROM c_filtered_claimed(1000) WHERE v = 999") == 1000);
}

TEST_CASE("C-API-V2: table function filter pushdown needs projection pushdown", "[capi_v2]") {
	EnvFixture fx;
	auto function = MakeFiltered(fx, "c_filtered_noproj", false, false);
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, &err) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_table_function_destroy(&function);
}

} // namespace test_capi_v2
