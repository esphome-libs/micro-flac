// Copyright 2026 Kevin Ahrendt
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "compiler.h"
#include "micro_flac/flac_decoder.h"

#include <cstddef>
#include <cstdint>

namespace micro_flac {

// Generate a bitmask with num_bits set to 1 (e.g., num_bits=3 -> 0b111 = 7)
// This replaces the UINT_MASK lookup table with bit manipulation for better performance
static FLAC_ALWAYS_INLINE uint32_t uint_mask(uint32_t num_bits) {
    return (num_bits >= 32) ? UINT32_MAX : ((1U << num_bits) - 1);
}

// Mask for bit buffer width (used to mask shift amounts)
static constexpr uint32_t BIT_BUFFER_SHIFT_MASK = BIT_BUFFER_BITS - 1;

// ============================================================================
// Local bit-reader state for the Rice partition hot loop
// ============================================================================
//
// Hoisting the decoder's bit-reader fields (bit_buffer_, bit_buffer_length_,
// buffer_, buffer_index_, bytes_left_) into a stack-local struct lets GCC
// promote them into registers across the sample loop. Without this, writes
// through out_ptr can alias the member fields in GCC's view, causing it to
// spill/reload bit_buffer_ (and others) on every iteration.
//
// These helpers are the single implementation of the bit-stream primitives.
// FLACDecoder::read_uint is a thin wrapper that copies state between `this->`
// and a stack-local struct around a call to read_uint_local.
// Rice-coded residual reads (decode_rice_partition) use read_rice_sint_local
// directly against a shared BitReaderLocal to avoid paying for an extra
// store/load across the resume/hot-loop boundary.
struct BitReaderLocal {
    bit_buffer_t bit_buffer;
    const uint8_t* buffer;
    size_t buffer_index;
    size_t bytes_left;
    uint32_t bit_buffer_length;
};

// Refill the bit buffer from the input stream. Returns true on out-of-data
// (zero bytes remaining); otherwise loads as many bytes as fit into
// bit_buffer, advances buffer_index/bytes_left, and sets bit_buffer_length.
//
// ESP-IDF disables jump tables by default (-fno-jump-tables), so a switch
// statement compiles to a chain of comparisons anyway. Using explicit
// if/else with FLAC_LIKELY on the hot path lets the compiler prioritize it.
//
// All paths overwrite bit_buffer with only the newly loaded bytes. Old bits
// are NOT preserved. This is safe because both callers handle old bits
// before calling refill:
//   - read_uint_local() extracts old bits into its local `result` before
//     calling refill
//   - read_rice_sint_local() only calls refill when bit_buffer_length == 0
//     (no old bits)
static FLAC_ALWAYS_INLINE bool refill_bit_buffer_local(BitReaderLocal& s) {
#if (BIT_BUFFER_BITS == 64)
    if (FLAC_LIKELY(s.bytes_left >= 8)) {
        s.bit_buffer = (static_cast<uint64_t>(s.buffer[s.buffer_index]) << 56) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 1]) << 48) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 2]) << 40) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 3]) << 32) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 4]) << 24) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 5]) << 16) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 6]) << 8) |
                       s.buffer[s.buffer_index + 7];
        s.buffer_index += 8;
        s.bytes_left -= 8;
        s.bit_buffer_length = 64;
        return false;
    }
    if (s.bytes_left == 7) {
        s.bit_buffer = (static_cast<uint64_t>(s.buffer[s.buffer_index]) << 48) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 1]) << 40) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 2]) << 32) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 3]) << 24) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 4]) << 16) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 5]) << 8) |
                       s.buffer[s.buffer_index + 6];
        s.buffer_index += 7;
        s.bit_buffer_length = 56;
        s.bytes_left = 0;
        return false;
    }
    if (s.bytes_left == 6) {
        s.bit_buffer = (static_cast<uint64_t>(s.buffer[s.buffer_index]) << 40) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 1]) << 32) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 2]) << 24) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 3]) << 16) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 4]) << 8) |
                       s.buffer[s.buffer_index + 5];
        s.buffer_index += 6;
        s.bit_buffer_length = 48;
        s.bytes_left = 0;
        return false;
    }
    if (s.bytes_left == 5) {
        s.bit_buffer = (static_cast<uint64_t>(s.buffer[s.buffer_index]) << 32) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 1]) << 24) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 2]) << 16) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 3]) << 8) |
                       s.buffer[s.buffer_index + 4];
        s.buffer_index += 5;
        s.bit_buffer_length = 40;
        s.bytes_left = 0;
        return false;
    }
    if (s.bytes_left == 4) {
        s.bit_buffer = (static_cast<uint64_t>(s.buffer[s.buffer_index]) << 24) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 1]) << 16) |
                       (static_cast<uint64_t>(s.buffer[s.buffer_index + 2]) << 8) |
                       s.buffer[s.buffer_index + 3];
        s.buffer_index += 4;
        s.bit_buffer_length = 32;
        s.bytes_left = 0;
        return false;
    }
#else
    if (FLAC_LIKELY(s.bytes_left >= 4)) {
        s.bit_buffer = (static_cast<uint32_t>(s.buffer[s.buffer_index]) << 24) |
                       (static_cast<uint32_t>(s.buffer[s.buffer_index + 1]) << 16) |
                       (static_cast<uint32_t>(s.buffer[s.buffer_index + 2]) << 8) |
                       s.buffer[s.buffer_index + 3];
        s.buffer_index += 4;
        s.bytes_left -= 4;
        s.bit_buffer_length = 32;
        return false;
    }
#endif
    if (s.bytes_left == 3) {
        s.bit_buffer = (static_cast<bit_buffer_t>(s.buffer[s.buffer_index]) << 16) |
                       (static_cast<bit_buffer_t>(s.buffer[s.buffer_index + 1]) << 8) |
                       s.buffer[s.buffer_index + 2];
        s.buffer_index += 3;
        s.bit_buffer_length = 24;
        s.bytes_left = 0;
        return false;
    }
    if (s.bytes_left == 2) {
        s.bit_buffer = (static_cast<bit_buffer_t>(s.buffer[s.buffer_index]) << 8) |
                       s.buffer[s.buffer_index + 1];
        s.buffer_index += 2;
        s.bit_buffer_length = 16;
        s.bytes_left = 0;
        return false;
    }
    if (s.bytes_left == 1) {
        s.bit_buffer = s.buffer[s.buffer_index];
        s.buffer_index += 1;
        s.bit_buffer_length = 8;
        s.bytes_left = 0;
        return false;
    }
    return true;
}

// Read an unsigned integer of `num_bits` bits using only the local state
// struct. On out-of-data, sets `out_of_data=true` and returns 0 without
// consuming any bits (bit_buffer/bit_buffer_length are left untouched in
// that case, matching the member read_uint contract).
static FLAC_ALWAYS_INLINE uint32_t read_uint_local(BitReaderLocal& s, uint8_t num_bits,
                                                   bool& out_of_data) {
    uint32_t result = 0;

    if (num_bits > s.bit_buffer_length) {
        const uint32_t new_bits_needed = num_bits - s.bit_buffer_length;
        const size_t bytes_needed = (new_bits_needed + 7) / 8;

        if (FLAC_UNLIKELY(s.bytes_left < bytes_needed)) {
            out_of_data = true;
            return 0;
        }

        if (new_bits_needed < BIT_BUFFER_BITS) {
            // Some of the current bits will be used in the result
            result = static_cast<uint32_t>(s.bit_buffer << new_bits_needed);
        }

        refill_bit_buffer_local(s);
        s.bit_buffer_length = s.bit_buffer_length - new_bits_needed;
    } else {
        s.bit_buffer_length -= num_bits;
    }

    result |= static_cast<uint32_t>(s.bit_buffer >> (s.bit_buffer_length & BIT_BUFFER_SHIFT_MASK));
    result &= uint_mask(num_bits);
    return result;
}

// Read a Rice-coded signed integer using only the local state struct.
// `mask` must equal `(1U << param) - 1` and is passed explicitly so the
// caller can pin it to a register across the partition loop (the inlined
// uint_mask otherwise tends to spill to the stack).
// On out-of-data, sets *out_of_data=true and writes partial-progress info
// into *unary_count_out and *binary_pending_out so the caller can persist
// it into the decoder's RiceState for resume.
//
// Template param `Resuming`:
//   false (hot path): start unary_count=0 and always run the unary phase.
//   true             : seed unary_count from `unary_count_in`, and skip the
//                      unary phase when `binary_pending_in` is true (the
//                      unary phase already completed on a prior call).
// With FLAC_ALWAYS_INLINE, the `Resuming` branches fold away at both call
// sites, so the hot-path instruction sequence is identical to the previous
// non-templated helper.
template <bool Resuming = false>
static FLAC_ALWAYS_INLINE int32_t read_rice_sint_local(
    BitReaderLocal& s, uint8_t param, uint32_t mask, bool* out_of_data, uint32_t* unary_count_out,
    bool* binary_pending_out, uint32_t unary_count_in = 0, bool binary_pending_in = false) {
    uint32_t unary_count = Resuming ? unary_count_in : 0;
    if (!Resuming || !binary_pending_in) {
        while (true) {
            if (s.bit_buffer_length == 0) {
                if (FLAC_UNLIKELY(refill_bit_buffer_local(s))) {
                    *unary_count_out = unary_count;
                    *binary_pending_out = false;
                    *out_of_data = true;
                    return 0;
                }
            }
            bit_buffer_t shifted = s.bit_buffer << (BIT_BUFFER_BITS - s.bit_buffer_length);
            if (FLAC_UNLIKELY(shifted == 0)) {
                unary_count += s.bit_buffer_length;
                s.bit_buffer_length = 0;
                continue;
            }
            uint32_t leading_zeros = static_cast<uint32_t>(FLAC_CLZ(shifted));
            unary_count += leading_zeros;
            s.bit_buffer_length = s.bit_buffer_length - (leading_zeros + 1);
            break;
        }
    }

    // Rice parameter is structurally bounded to < 32 by the FLAC spec
    // (RFC 9639 §9.2.7): the partition parameter is read as a 4-bit or 5-bit
    // field (residual coding method 0 or 1), giving max values of 15 and 31
    // respectively. For method 1, value 31 is the escape marker; this
    // function is only invoked from the non-escape branch, so param is
    // guaranteed <= 30. Even with corrupted input, read_uint(N) physically
    // cannot return more than 2^N - 1, so the invariant holds regardless.
    //
    // Hinting this to the compiler lets it drop the `num_bits >= 32` guard
    // inside uint_mask / read_uint_local, which in turn lets register
    // allocation keep `buffer_` pinned in a register (previously occupied by
    // the constant 31) and eliminates a per-sample conditional mask reload.
    //
    // Safety note: this assumption is also relied on by `(unary_count << param)`
    // below, which is UB for param >= 32. We are making an already-required
    // invariant explicit to the optimizer, not introducing a new one.
    FLAC_ASSUME(param < 32);

    // Binary phase. Inline the fast path so the precomputed `mask` is used
    // directly instead of going through uint_mask (which, even with the
    // unreachable hint above, tends to spill the computed mask to the stack).
    uint32_t binary = 0;
    if (FLAC_LIKELY(param <= s.bit_buffer_length)) {
        s.bit_buffer_length = s.bit_buffer_length - param;
        binary =
            static_cast<uint32_t>(s.bit_buffer >> (s.bit_buffer_length & BIT_BUFFER_SHIFT_MASK)) &
            mask;
    } else {
        // Slow path: binary field straddles a refill. Inline read_uint here so
        // we can reuse the caller-provided `mask` instead of rebuilding it via
        // uint_mask (which tends to spill). Since param < 32 and
        // bit_buffer_length >= 0, new_bits_needed is in [1, 31] (always
        // strictly less than BIT_BUFFER_BITS on both 32- and 64-bit hosts), so
        // we can skip the read_uint_local `new_bits_needed >= BIT_BUFFER_BITS`
        // edge case.
        const uint32_t new_bits_needed = param - s.bit_buffer_length;
        const size_t bytes_needed = (new_bits_needed + 7) / 8;
        if (FLAC_UNLIKELY(s.bytes_left < bytes_needed)) {
            *unary_count_out = unary_count;
            *binary_pending_out = true;
            *out_of_data = true;
            return 0;
        }
        const uint32_t high = static_cast<uint32_t>(s.bit_buffer << new_bits_needed);
        refill_bit_buffer_local(s);
        s.bit_buffer_length = s.bit_buffer_length - new_bits_needed;
        binary = (high | static_cast<uint32_t>(s.bit_buffer >>
                                               (s.bit_buffer_length & BIT_BUFFER_SHIFT_MASK))) &
                 mask;
    }

    uint32_t value = (unary_count << param) | binary;
    return static_cast<int32_t>((value >> 1) ^ -(value & 1));
}

}  // namespace micro_flac
