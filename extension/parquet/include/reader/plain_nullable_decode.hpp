//===----------------------------------------------------------------------===//
//                         DuckDB
//
// reader/plain_nullable_decode.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/bswap.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/types/validity_mask.hpp"
#include "resizable_buffer.hpp"

namespace duckdb {

//! Bit i of the result is set if levels[i] is one, for eight levels (exact for any byte value)
static inline uint64_t ParquetValidBits8(const uint8_t *levels) {
	const uint64_t y = BSwapIfBE(Load<uint64_t>(levels)) ^ 0x0101010101010101ULL;
	// the high bit of each byte of y is set if the byte is not zero, and no carry leaves a byte
	const uint64_t not_one = (((y & 0x7F7F7F7F7F7F7F7FULL) + 0x7F7F7F7F7F7F7F7FULL) | y) & 0x8080808080808080ULL;
	// the multiplier moves bit 8i+7 to bit 56+i, no two partial products overlap
	return ~(((not_one >> 7) * 0x0102040810204080ULL) >> 56) & 0xFF;
}

//! Core has no popcount, only the loop in ValidityMask::CountValid
static inline idx_t ParquetCountBits(uint64_t bits) {
#if defined(__GNUC__) || defined(__clang__)
	return static_cast<idx_t>(__builtin_popcountll(bits));
#else
	idx_t count = 0;
	for (; bits; bits &= bits - 1) {
		count++;
	}
	return count;
#endif
}

//! Decodes a plain page of fixed-size values for a column with a maximum definition level of one and no repeats. The
//! levels are read 64 at a time into one validity word, and the values of the valid rows are copied. The slot of a NULL
//! row is not written.
template <class VALUE_TYPE>
static void ParquetDecodeNullablePlain(ByteBuffer &plain_data, const uint8_t *defines, const idx_t num_values,
                                       const idx_t result_offset, VALUE_TYPE *result_ptr, ValidityMask &result_mask) {
	const idx_t end = result_offset + num_values;
	D_ASSERT(end <= result_mask.Capacity());
	uint8_t padded[64];
	for (idx_t row = result_offset; row < end;) {
		// a step never crosses a validity word
		const idx_t shift = row % 64;
		const idx_t count = MinValue<idx_t>(64 - shift, end - row);
		const uint64_t step_mask = count == 64 ? ~uint64_t(0) : (uint64_t(1) << count) - 1;
		// the gather reads 64 levels, so a short step reads them from a zero-padded copy (padding is never valid)
		const uint8_t *levels = defines + row;
		if (count < 64) {
			memset(padded, 0, sizeof(padded));
			memcpy(padded, levels, count);
			levels = padded;
		}
		uint64_t valid = 0;
		for (idx_t i = 0; i < 8; i++) {
			valid |= ParquetValidBits8(levels + i * 8) << (i * 8);
		}
		if (valid != step_mask) {
			// like SetInvalid this only clears bits
			result_mask.EnsureWritable();
			result_mask.GetValidityEntryUnsafe(row / 64) &= ~((~valid & step_mask) << shift);
		}
		const idx_t valid_count = ParquetCountBits(valid);
		plain_data.Available(valid_count * sizeof(VALUE_TYPE));
		const data_t *src = plain_data.GetCurrentLoc();
		plain_data.UnsafeInc(valid_count * sizeof(VALUE_TYPE));
		VALUE_TYPE *target = result_ptr + row;
		if (valid_count == count) {
			memcpy(target, src, count * sizeof(VALUE_TYPE));
		} else {
			for (uint64_t bits = valid; bits; bits &= bits - 1) {
				target[CountZeros<uint64_t>::Trailing(bits)] = Load<VALUE_TYPE>(src);
				src += sizeof(VALUE_TYPE);
			}
		}
		row += count;
	}
}

} // namespace duckdb
