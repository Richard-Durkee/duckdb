#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

namespace duckdb {

unique_ptr<ExpressionState> ExpressionExecutor::InitializeState(const BoundOperatorExpression &expr,
                                                                ExpressionExecutorState &root) {
	auto result = make_uniq<ExpressionState>(expr, root);
	const auto save_inside_try = root.inside_try;
	if (expr.GetExpressionType() == ExpressionType::OPERATOR_TRY) {
		root.inside_try = true;
	}
	for (auto &child : expr.GetChildren()) {
		result->AddChild(*child);
	}
	root.inside_try = save_inside_try;

	result->Finalize();
	return result;
}

template <bool HAS_VALID_SEL, bool HAS_NULL_SEL>
static idx_t SelectNullLoop(const VectorValidityIterator &entries, const SelectionVector &sel, idx_t count,
                            optional_ptr<SelectionVector> valid_sel, optional_ptr<SelectionVector> null_sel) {
	idx_t valid_count = 0, null_count = 0;
	for (idx_t i = 0; i < count; i++) {
		const auto row_idx = sel.get_index(i);
		const auto valid = entries.IsValid(i);
		if (HAS_VALID_SEL) {
			valid_sel->set_index(valid_count, row_idx);
			valid_count += valid;
		}
		if (HAS_NULL_SEL) {
			null_sel->set_index(null_count, row_idx);
			null_count += !valid;
		}
	}
	return HAS_VALID_SEL ? valid_count : count - null_count;
}

static idx_t SelectNulls(const Vector &input, const SelectionVector &sel, idx_t count,
                         optional_ptr<SelectionVector> valid_sel, optional_ptr<SelectionVector> null_sel) {
	auto entries = input.Validity();
	if (entries.CannotHaveNull() || input.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		const auto valid = entries.CannotHaveNull() || entries.IsValid(0);
		auto result_sel = valid ? valid_sel : null_sel;
		if (result_sel) {
			for (idx_t i = 0; i < count; i++) {
				result_sel->set_index(i, sel.get_index(i));
			}
		}
		return valid ? count : 0;
	}
	if (valid_sel && null_sel) {
		return SelectNullLoop<true, true>(entries, sel, count, valid_sel, null_sel);
	}
	if (valid_sel) {
		return SelectNullLoop<true, false>(entries, sel, count, valid_sel, null_sel);
	}
	return SelectNullLoop<false, true>(entries, sel, count, valid_sel, null_sel);
}

idx_t ExpressionExecutor::Select(const BoundOperatorExpression &expr, ExpressionState &state,
                                 optional_ptr<const SelectionVector> sel, idx_t count,
                                 optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel) {
	const auto type = expr.GetExpressionType();
	if (type != ExpressionType::OPERATOR_IS_NULL && type != ExpressionType::OPERATOR_IS_NOT_NULL) {
		return DefaultSelect(expr, &state, sel.get(), count, true_sel.get(), false_sel.get());
	}
	D_ASSERT(expr.GetChildren().size() == 1);
	state.intermediate_chunk.Reset();
	auto &child = state.intermediate_chunk.data[0];
	Execute(*expr.GetChildren()[0], state.child_states[0].get(), sel.get(), count, child);

	const SelectionVector identity;
	const auto &input_sel = sel ? *sel : identity;
	if (type == ExpressionType::OPERATOR_IS_NOT_NULL) {
		return SelectNulls(child, input_sel, count, true_sel, false_sel);
	}
	return count - SelectNulls(child, input_sel, count, false_sel, true_sel);
}

template <class T, bool IS_NOT_IN>
static void ExecuteConstantInList(const BoundOperatorExpression &expr, const Vector &left, Vector &result,
                                  idx_t count) {
	vector<T> constants;
	bool has_null_constant = false;
	for (idx_t child = 1; child < expr.GetChildren().size(); child++) {
		auto &value = expr.GetChildren()[child]->Cast<BoundConstantExpression>().GetValue();
		if (value.IsNull()) {
			has_null_constant = true;
		} else {
			constants.push_back(value.GetValueUnsafe<T>());
		}
	}
	if (left.GetVectorType() == VectorType::FLAT_VECTOR) {
		// compare all rows against one constant at a time so the comparison loops vectorize
		auto data = FlatVector::GetData<T>(left);
		auto &left_validity = FlatVector::Validity(left);
		result.SetVectorType(VectorType::FLAT_VECTOR);
		FlatVector::SetSize(result, count);
		auto result_data = FlatVector::GetDataMutable<bool>(result);
		for (idx_t i = 0; i < count; i++) {
			result_data[i] = false;
		}
		for (auto &constant : constants) {
			for (idx_t i = 0; i < count; i++) {
				result_data[i] |= Equals::Operation(data[i], constant);
			}
		}
		auto &result_validity = FlatVector::ValidityMutable(result);
		if (has_null_constant) {
			// a NULL in the list makes the result NULL for rows that do not match any other entry
			result_validity.Copy(left_validity, count);
			for (idx_t i = 0; i < count; i++) {
				if (!result_data[i]) {
					result_validity.SetInvalid(i);
				}
			}
		} else {
			result_validity.Initialize(left_validity);
		}
		if (IS_NOT_IN) {
			for (idx_t i = 0; i < count; i++) {
				result_data[i] = !result_data[i];
			}
		}
		return;
	}
	auto contains = [&](const T &input) {
		bool found = false;
		for (auto &constant : constants) {
			found |= Equals::Operation(input, constant);
		}
		return found;
	};
	if (has_null_constant) {
		// a NULL in the list makes the result NULL for rows that do not match any other entry
		UnaryExecutor::Execute<T, bool>(
		    left, result, count,
		    [&](const T &input) -> optional<bool> {
			    if (contains(input)) {
				    return !IS_NOT_IN;
			    }
			    return nullopt;
		    },
		    FunctionErrors::CANNOT_ERROR);
	} else {
		UnaryExecutor::Execute<T, bool>(
		    left, result, count, [&](const T &input) { return contains(input) != IS_NOT_IN; },
		    FunctionErrors::CANNOT_ERROR);
	}
}

template <bool IS_NOT_IN>
static bool TryExecuteConstantInList(const BoundOperatorExpression &expr, const Vector &left, Vector &result,
                                     idx_t count) {
	for (idx_t child = 1; child < expr.GetChildren().size(); child++) {
		auto &child_expr = *expr.GetChildren()[child];
		if (child_expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
		    child_expr.GetReturnType() != left.GetType()) {
			return false;
		}
	}
	switch (left.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		ExecuteConstantInList<int8_t, IS_NOT_IN>(expr, left, result, count);
		return true;
	case PhysicalType::INT16:
		ExecuteConstantInList<int16_t, IS_NOT_IN>(expr, left, result, count);
		return true;
	case PhysicalType::INT32:
		ExecuteConstantInList<int32_t, IS_NOT_IN>(expr, left, result, count);
		return true;
	case PhysicalType::INT64:
		ExecuteConstantInList<int64_t, IS_NOT_IN>(expr, left, result, count);
		return true;
	case PhysicalType::INT128:
		ExecuteConstantInList<hugeint_t, IS_NOT_IN>(expr, left, result, count);
		return true;
	case PhysicalType::UINT8:
		ExecuteConstantInList<uint8_t, IS_NOT_IN>(expr, left, result, count);
		return true;
	case PhysicalType::UINT16:
		ExecuteConstantInList<uint16_t, IS_NOT_IN>(expr, left, result, count);
		return true;
	case PhysicalType::UINT32:
		ExecuteConstantInList<uint32_t, IS_NOT_IN>(expr, left, result, count);
		return true;
	case PhysicalType::UINT64:
		ExecuteConstantInList<uint64_t, IS_NOT_IN>(expr, left, result, count);
		return true;
	case PhysicalType::UINT128:
		ExecuteConstantInList<uhugeint_t, IS_NOT_IN>(expr, left, result, count);
		return true;
	case PhysicalType::FLOAT:
		ExecuteConstantInList<float, IS_NOT_IN>(expr, left, result, count);
		return true;
	case PhysicalType::DOUBLE:
		ExecuteConstantInList<double, IS_NOT_IN>(expr, left, result, count);
		return true;
	case PhysicalType::INTERVAL:
		ExecuteConstantInList<interval_t, IS_NOT_IN>(expr, left, result, count);
		return true;
	case PhysicalType::VARCHAR:
		ExecuteConstantInList<string_t, IS_NOT_IN>(expr, left, result, count);
		return true;
	default:
		return false;
	}
}

void ExpressionExecutor::Execute(const BoundOperatorExpression &expr, ExpressionState *state,
                                 const SelectionVector *sel, idx_t count, Vector &result) {
	// special handling for special snowflake 'IN'
	// IN has n children
	auto expression_type = expr.GetExpressionType();
	if (expression_type == ExpressionType::COMPARE_IN || expression_type == ExpressionType::COMPARE_NOT_IN) {
		if (expr.GetChildren().size() < 2) {
			throw InvalidInputException("IN needs at least two children");
		}

		Vector left(expr.GetChildren()[0]->GetReturnType());
		// eval left side
		Execute(*expr.GetChildren()[0], state->child_states[0].get(), sel, count, left);

		// fast path: compare against a list of constants in a single pass
		if (expression_type == ExpressionType::COMPARE_NOT_IN
		        ? TryExecuteConstantInList<true>(expr, left, result, count)
		        : TryExecuteConstantInList<false>(expr, left, result, count)) {
			return;
		}

		// init result to false
		Vector intermediate(LogicalType::BOOLEAN);
		intermediate.Reference(Value::BOOLEAN(false), count_t(count));

		// in rhs is a list of constants
		// for every child, OR the result of the comparison with the left
		// to get the overall result.
		for (idx_t child = 1; child < expr.GetChildren().size(); child++) {
			Vector vector_to_check(expr.GetChildren()[child]->GetReturnType());
			Vector comp_res(LogicalType::BOOLEAN);

			Execute(*expr.GetChildren()[child], state->child_states[child].get(), sel, count, vector_to_check);
			VectorOperations::Equals(left, vector_to_check, comp_res);

			if (child == 1) {
				// first child: move to result
				intermediate.Reference(comp_res);
			} else {
				// otherwise OR together
				Vector new_result(LogicalType::BOOLEAN);
				VectorOperations::Or(intermediate, comp_res, new_result);
				intermediate.Reference(new_result);
			}
		}
		if (expression_type == ExpressionType::COMPARE_NOT_IN) {
			// NOT IN: invert result
			VectorOperations::Not(intermediate, result);
		} else {
			// directly use the result
			result.Reference(intermediate);
		}
	} else if (expression_type == ExpressionType::OPERATOR_COALESCE) {
		SelectionVector sel_a(count);
		SelectionVector sel_b(count);
		SelectionVector slice_sel(count);
		SelectionVector result_sel(count);
		SelectionVector *next_sel = &sel_a;
		const SelectionVector *current_sel = sel;
		idx_t remaining_count = count;
		idx_t next_count;
		for (idx_t child = 0; child < expr.GetChildren().size(); child++) {
			Vector vector_to_check(expr.GetChildren()[child]->GetReturnType());
			Execute(*expr.GetChildren()[child], state->child_states[child].get(), current_sel, remaining_count,
			        vector_to_check);

			auto entries = vector_to_check.Validity();
			idx_t result_count = 0;
			next_count = 0;
			for (idx_t i = 0; i < remaining_count; i++) {
				auto base_idx = current_sel ? current_sel->get_index(i) : i;
				if (entries.IsValid(i)) {
					slice_sel.set_index(result_count, i);
					result_sel.set_index(result_count++, base_idx);
				} else {
					next_sel->set_index(next_count++, base_idx);
				}
			}
			if (result_count > 0) {
				vector_to_check.Slice(slice_sel, result_count);
				FillSwitch(vector_to_check, result, result_sel, NumericCast<sel_t>(result_count));
			}
			current_sel = next_sel;
			next_sel = next_sel == &sel_a ? &sel_b : &sel_a;
			remaining_count = next_count;
			if (next_count == 0) {
				break;
			}
		}
		if (remaining_count > 0) {
			for (idx_t i = 0; i < remaining_count; i++) {
				FlatVector::SetNull(result, current_sel->get_index(i), true);
			}
		}
		if (sel) {
			result.Slice(*sel, count);
		} else if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	} else if (expression_type == ExpressionType::OPERATOR_TRY) {
		auto &child_state = *state->child_states[0];
		Vector try_result(result.GetType());
		try {
			Execute(*expr.GetChildren()[0], &child_state, sel, count, try_result);
			if (try_result.GetVectorType() == VectorType::CONSTANT_VECTOR) {
				result.Reference(try_result);
				return;
			}
			VectorOperations::Copy(try_result, result, count, 0, 0);
			return;
		} catch (std::exception &ex) {
			ErrorData error(ex);
			auto error_type = error.Type();
			if (!Exception::IsExecutionError(error_type)) {
				throw;
			}
		}

		// On error, evaluate per row
		// CASE/COALESCE write their result at the physical row index, so the intermediate must fit that index
		SelectionVector selvec(1);
		DataChunk intermediate;
		intermediate.Initialize(GetAllocator(), {result.GetType()}, STANDARD_VECTOR_SIZE);
		for (idx_t i = 0; i < count; i++) {
			intermediate.Reset();
			intermediate.SetChildCardinality(1);

			// Make sure to clear any dictionary states in the child expression, so that it actually
			// gets executed anew for every row
			child_state.ResetDictionaryStates();

			selvec.set_index(0, sel ? sel->get_index(i) : i);
			Value val(result.GetType());
			try {
				Execute(*expr.GetChildren()[0], &child_state, &selvec, 1, intermediate.data[0]);
				val = intermediate.GetValue(0, 0);
			} catch (std::exception &ex) {
				ErrorData error(ex);
				auto error_type = error.Type();
				if (!Exception::IsExecutionError(error_type)) {
					throw;
				}
			}
			result.SetValue(i, val);
		}
		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	} else if (expr.GetChildren().size() == 1) {
		state->intermediate_chunk.Reset();
		auto &child = state->intermediate_chunk.data[0];

		Execute(*expr.GetChildren()[0], state->child_states[0].get(), sel, count, child);
		switch (expr.GetExpressionType()) {
		case ExpressionType::OPERATOR_NOT: {
			VectorOperations::Not(child, result);
			break;
		}
		case ExpressionType::OPERATOR_IS_NULL: {
			VectorOperations::IsNull(child, result);
			break;
		}
		case ExpressionType::OPERATOR_IS_NOT_NULL: {
			VectorOperations::IsNotNull(child, result);
			break;
		}
		default:
			throw NotImplementedException("Unsupported operator type with 1 child!");
		}
	} else {
		throw NotImplementedException("operator");
	}
}

} // namespace duckdb
