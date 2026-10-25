#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

void VectorOperations::DistinctFrom(const Vector &left, const Vector &right, Vector &result) {
	D_ASSERT(result.GetType() == LogicalType::BOOLEAN);
	Vector comparator_result(LogicalType::TINYINT);
	VectorOperations::DistinctComparator(left, right, comparator_result);
	const idx_t count = comparator_result.size();
	auto cmp_data = comparator_result.Values<int8_t>();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::Writer<bool>(result, count);
	for (idx_t i = 0; i < count; i++) {
		result_data.WriteValue(cmp_data[i].GetValueUnsafe() != 0);
	}
}

void VectorOperations::NotDistinctFrom(const Vector &left, const Vector &right, Vector &result) {
	D_ASSERT(result.GetType() == LogicalType::BOOLEAN);
	Vector comparator_result(LogicalType::TINYINT);
	VectorOperations::DistinctComparator(left, right, comparator_result);
	const idx_t count = comparator_result.size();
	auto cmp_data = comparator_result.Values<int8_t>();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::Writer<bool>(result, count);
	for (idx_t i = 0; i < count; i++) {
		result_data.WriteValue(cmp_data[i].GetValueUnsafe() == 0);
	}
}

template <bool HAS_TRUE_SEL, bool HAS_FALSE_SEL, class PREDICATE>
static idx_t DistinctComparatorSelectLoop(const int8_t *cmp_data, const SelectionVector &sel, idx_t count,
                                          optional_ptr<SelectionVector> true_sel,
                                          optional_ptr<SelectionVector> false_sel, PREDICATE predicate) {
	// write to both selections unconditionally to avoid a branch on the (unpredictable) result
	idx_t true_count = 0;
	idx_t false_count = 0;
	for (idx_t i = 0; i < count; i++) {
		const auto result_idx = sel.get_index(i);
		const bool result = predicate(cmp_data[i]);
		if (HAS_TRUE_SEL) {
			true_sel->set_index(true_count, result_idx);
		}
		if (HAS_FALSE_SEL) {
			false_sel->set_index(false_count, result_idx);
		}
		true_count += result;
		false_count += !result;
	}
	return true_count;
}

template <class FILL_FN, class PREDICATE>
static idx_t DistinctComparatorSelect(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
                                      idx_t count, optional_ptr<SelectionVector> true_sel,
                                      optional_ptr<SelectionVector> false_sel, FILL_FN fill_fn, PREDICATE predicate) {
	Vector comparator_result(LogicalType::TINYINT, count);
	fill_fn(left, right, comparator_result, count);
	comparator_result.Flatten();
	auto cmp_data = FlatVector::GetData<int8_t>(comparator_result);
	if (!sel) {
		sel = FlatVector::IncrementalSelectionVector();
	}
	if (true_sel && false_sel) {
		return DistinctComparatorSelectLoop<true, true>(cmp_data, *sel, count, true_sel, false_sel, predicate);
	} else if (true_sel) {
		return DistinctComparatorSelectLoop<true, false>(cmp_data, *sel, count, true_sel, false_sel, predicate);
	} else if (false_sel) {
		return DistinctComparatorSelectLoop<false, true>(cmp_data, *sel, count, true_sel, false_sel, predicate);
	}
	return DistinctComparatorSelectLoop<false, false>(cmp_data, *sel, count, true_sel, false_sel, predicate);
}

// true := A != B with nulls being equal
idx_t VectorOperations::DistinctFrom(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
                                     idx_t count, optional_ptr<SelectionVector> true_sel,
                                     optional_ptr<SelectionVector> false_sel) {
	return DistinctComparatorSelect(left, right, sel, count, true_sel, false_sel,
	                                VectorOperations::DistinctComparatorFill, [](int8_t v) { return v != 0; });
}
// true := A == B with nulls being equal
idx_t VectorOperations::NotDistinctFrom(const Vector &left, const Vector &right,
                                        optional_ptr<const SelectionVector> sel, idx_t count,
                                        optional_ptr<SelectionVector> true_sel,
                                        optional_ptr<SelectionVector> false_sel) {
	return DistinctComparatorSelect(left, right, sel, count, true_sel, false_sel,
	                                VectorOperations::DistinctComparatorFill, [](int8_t v) { return v == 0; });
}

// true := A > B with nulls being maximal
idx_t VectorOperations::DistinctGreaterThan(const Vector &left, const Vector &right,
                                            optional_ptr<const SelectionVector> sel, idx_t count,
                                            optional_ptr<SelectionVector> true_sel,
                                            optional_ptr<SelectionVector> false_sel,
                                            optional_ptr<ValidityMask> null_mask) {
	return DistinctComparatorSelect(left, right, sel, count, true_sel, false_sel,
	                                VectorOperations::DistinctComparatorFill, [](int8_t v) { return v > 0; });
}

// true := A > B with nulls being minimal
idx_t VectorOperations::DistinctGreaterThanNullsFirst(const Vector &left, const Vector &right,
                                                      optional_ptr<const SelectionVector> sel, idx_t count,
                                                      optional_ptr<SelectionVector> true_sel,
                                                      optional_ptr<SelectionVector> false_sel,
                                                      optional_ptr<ValidityMask> null_mask) {
	return DistinctComparatorSelect(left, right, sel, count, true_sel, false_sel,
	                                VectorOperations::DistinctComparatorNullsFirstFill, [](int8_t v) { return v > 0; });
}

// true := A >= B with nulls being maximal
idx_t VectorOperations::DistinctGreaterThanEquals(const Vector &left, const Vector &right,
                                                  optional_ptr<const SelectionVector> sel, idx_t count,
                                                  optional_ptr<SelectionVector> true_sel,
                                                  optional_ptr<SelectionVector> false_sel,
                                                  optional_ptr<ValidityMask> null_mask) {
	return DistinctComparatorSelect(left, right, sel, count, true_sel, false_sel,
	                                VectorOperations::DistinctComparatorFill, [](int8_t v) { return v >= 0; });
}
// true := A < B with nulls being maximal
idx_t VectorOperations::DistinctLessThan(const Vector &left, const Vector &right,
                                         optional_ptr<const SelectionVector> sel, idx_t count,
                                         optional_ptr<SelectionVector> true_sel,
                                         optional_ptr<SelectionVector> false_sel,
                                         optional_ptr<ValidityMask> null_mask) {
	return DistinctComparatorSelect(left, right, sel, count, true_sel, false_sel,
	                                VectorOperations::DistinctComparatorFill, [](int8_t v) { return v < 0; });
}

// true := A < B with nulls being minimal
idx_t VectorOperations::DistinctLessThanNullsFirst(const Vector &left, const Vector &right,
                                                   optional_ptr<const SelectionVector> sel, idx_t count,
                                                   optional_ptr<SelectionVector> true_sel,
                                                   optional_ptr<SelectionVector> false_sel,
                                                   optional_ptr<ValidityMask> null_mask) {
	return DistinctComparatorSelect(left, right, sel, count, true_sel, false_sel,
	                                VectorOperations::DistinctComparatorNullsFirstFill, [](int8_t v) { return v < 0; });
}

// true := A <= B with nulls being maximal
idx_t VectorOperations::DistinctLessThanEquals(const Vector &left, const Vector &right,
                                               optional_ptr<const SelectionVector> sel, idx_t count,
                                               optional_ptr<SelectionVector> true_sel,
                                               optional_ptr<SelectionVector> false_sel,
                                               optional_ptr<ValidityMask> null_mask) {
	return DistinctComparatorSelect(left, right, sel, count, true_sel, false_sel,
	                                VectorOperations::DistinctComparatorFill, [](int8_t v) { return v <= 0; });
}

} // namespace duckdb
