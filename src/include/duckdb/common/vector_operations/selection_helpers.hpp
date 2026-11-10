//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector_operations/selection_helpers.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace duckdb {

//! Limit boolean staging to narrow integers; wider inputs can lose to scalar selection.
template <class... ARGS>
constexpr bool UseBooleanSelectionBatch() {
	return ((std::is_integral<ARGS>::value && sizeof(ARGS) <= sizeof(uint32_t)) && ...);
}

//! Masked batches also compare NULL rows, whose bool payloads need not be 0 or 1.
template <class... ARGS>
constexpr bool UseMaskedBooleanSelectionBatch() {
	return UseBooleanSelectionBatch<ARGS...>() && (!std::is_same<ARGS, bool>::value && ...);
}

//! Clear the 0/1 matches of the 64 rows whose validity bit is unset.
inline void MaskBooleanSelection(uint8_t *matches, uint64_t validity_entry) {
	for (uint64_t byte_idx = 0; byte_idx < 8; byte_idx++) {
		// Spread the eight validity bits into the low bit of eight bytes.
		const auto bits = (validity_entry >> (byte_idx * 8)) & 0xFF;
		const auto mask =
		    (((bits & 0x7F) * UINT64_C(0x0002040810204081)) & UINT64_C(0x0101010101010101)) | ((bits >> 7) << 56);
		uint64_t word;
		memcpy(&word, matches + byte_idx * 8, sizeof(word));
		word &= mask;
		memcpy(matches + byte_idx * 8, &word, sizeof(word));
	}
}

} // namespace duckdb
