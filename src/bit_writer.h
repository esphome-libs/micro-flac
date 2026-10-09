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

/// @file bit_writer.h
/// @brief MSB-first bit writer for FLAC frame encoding
///
/// Mirrors bit_reader.h: stack-local state (BitWriterLocal) the compiler can
/// keep in registers across a hot loop, and FLAC_ALWAYS_INLINE functions on
/// it. CRCs are computed over the finished bytes, not here.

#pragma once

#include "compiler.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace micro_flac {

// The accumulator, one machine word (BIT_BUFFER_BITS) wide
#if BIT_BUFFER_BITS == 32
using bit_writer_acc_t = uint32_t;
#else
using bit_writer_acc_t = uint64_t;
#endif

// ============================================================================
// Local bit-writer state
// ============================================================================
//
// Between calls, `acc` holds at most one partial byte (nbits < 8) in its top
// bits, and every bit below them is zero. That lets write_zeros() store whole
// zero bytes with memset() once aligned.
struct BitWriterLocal {
    bit_writer_acc_t acc;  // Pending bits, left-justified (MSB-first) in the top `nbits` bits
    uint32_t nbits;        // Valid bits in acc
    uint8_t* pos;          // Next byte to write in the output buffer
    uint8_t* end;          // One-past-the-last valid byte in the output buffer
    bool overflow;         // Set once a write would pass `end`; later writes become no-ops
};

// The low `num_bits` bits set (up to 32). Named apart from bit_reader.h's
// uint_mask() so both headers can share a translation unit.
static FLAC_ALWAYS_INLINE uint32_t write_uint_mask(uint8_t num_bits) {
    return (num_bits >= 32) ? UINT32_MAX : ((1U << num_bits) - 1U);
}

// Initialize a writer over [buffer, buffer + capacity)
static FLAC_ALWAYS_INLINE void writer_init(BitWriterLocal& bw, uint8_t* buffer, size_t capacity) {
    bw.acc = 0;
    bw.nbits = 0;
    bw.pos = buffer;
    bw.end = buffer + capacity;
    bw.overflow = false;
}

// Store the complete bytes at the top of `acc`, setting `overflow` instead of
// writing past `end`
static FLAC_ALWAYS_INLINE void flush_full_bytes(BitWriterLocal& bw) {
    while (bw.nbits >= 8) {
        if (FLAC_UNLIKELY(bw.pos >= bw.end)) {
            bw.overflow = true;
            bw.acc = 0;
            bw.nbits = 0;
            return;
        }
        *bw.pos++ = static_cast<uint8_t>(bw.acc >> (BIT_BUFFER_BITS - 8));
        bw.acc = static_cast<bit_writer_acc_t>(bw.acc << 8);
        bw.nbits -= 8;
    }
}

// Write the low `num_bits` (0-32) bits of `value`. Splits into two pieces
// only on a 32-bit accumulator.
static FLAC_ALWAYS_INLINE void write_uint(BitWriterLocal& bw, uint32_t value, uint8_t num_bits) {
    if (FLAC_UNLIKELY(bw.overflow) || num_bits == 0) {
        return;
    }

    const uint32_t masked = value & write_uint_mask(num_bits);
    uint32_t remaining = num_bits;

    while (remaining > 0) {
        const uint32_t space = BIT_BUFFER_BITS - bw.nbits;
        const uint32_t chunk = (remaining < space) ? remaining : space;
        // The next `chunk` bits. remaining > chunk only happens with a 32-bit
        // accumulator, which cppcheck's host analysis cannot see.
        // cppcheck-suppress knownConditionTrueFalse
        const uint32_t chunk_bits = (remaining > chunk) ? (masked >> (remaining - chunk)) : masked;
        bw.acc |=
            static_cast<bit_writer_acc_t>(chunk_bits & write_uint_mask(static_cast<uint8_t>(chunk)))
            << (space - chunk);
        bw.nbits += chunk;
        remaining -= chunk;

        if (bw.nbits >= 8) {
            flush_full_bytes(bw);
            if (FLAC_UNLIKELY(bw.overflow)) {
                return;
            }
        }
    }
}

// Write `count` zero bits, as whole bytes once aligned, which keeps long Rice
// unary runs cheap
static FLAC_ALWAYS_INLINE void write_zeros(BitWriterLocal& bw, uint32_t count) {
    if (FLAC_UNLIKELY(bw.overflow) || count == 0) {
        return;
    }

    if (bw.nbits != 0) {
        const uint32_t space_in_byte = 8 - bw.nbits;
        const uint32_t chunk = (count < space_in_byte) ? count : space_in_byte;
        bw.nbits += chunk;  // acc's low bits are already zero
        count -= chunk;
        if (bw.nbits < 8) {
            // Still within the partial byte; the code below would reset nbits
            return;
        }
        flush_full_bytes(bw);
        if (FLAC_UNLIKELY(bw.overflow)) {
            return;
        }
    }

    // Byte-aligned now
    const uint32_t whole_bytes = count / 8;
    if (whole_bytes != 0) {
        const size_t available = static_cast<size_t>(bw.end - bw.pos);
        if (FLAC_UNLIKELY(whole_bytes > available)) {
            memset(bw.pos, 0, available);
            bw.pos = bw.end;
            bw.overflow = true;
            return;
        }
        memset(bw.pos, 0, whole_bytes);
        bw.pos += whole_bytes;
    }

    bw.nbits = count - (whole_bytes * 8);  // acc's low bits already zero
}

// Write one Rice code at parameter k (0-30; RFC 9639 SS9.2.7): the zigzagged
// value u as u >> k zero bits, a stop bit, then u's low k bits. When the
// whole code fits the accumulator, as it nearly always does, it is one OR
// and shift.
static FLAC_ALWAYS_INLINE void write_rice(BitWriterLocal& bw, int32_t residual, uint8_t k) {
    if (FLAC_UNLIKELY(bw.overflow)) {
        return;
    }

    // Zigzag, the inverse of read_rice_sint_local()'s mapping. The signed
    // shift is implementation-defined before C++20; every target here
    // sign-extends.
    const uint32_t u =
        // cppcheck-suppress shiftTooManyBitsSigned
        // cppcheck-suppress shiftNegativeLHS
        (static_cast<uint32_t>(residual) << 1) ^ static_cast<uint32_t>(residual >> 31);

    const uint32_t q = u >> k;
    const uint32_t space = BIT_BUFFER_BITS - bw.nbits;
    // q < space also keeps q + 1 + k from wrapping
    const uint32_t total_bits = q + 1U + k;
    // NOLINTNEXTLINE(readability-simplify-boolean-expr) -- FLAC_LIKELY's !!(x)
    if (FLAC_LIKELY(q < space && total_bits <= space)) {
        const bit_writer_acc_t code =
            (static_cast<bit_writer_acc_t>(1) << k) | (u & write_uint_mask(k));
        bw.acc |= code << (space - total_bits);
        bw.nbits += total_bits;
        if (bw.nbits >= 8) {
            flush_full_bytes(bw);
        }
        return;
    }

    write_zeros(bw, q);
    write_uint(bw, 1, 1);
    if (k > 0) {
        write_uint(bw, u & write_uint_mask(k), k);
    }
}

// write_rice() for `count` residuals with no bounds checks: the caller must
// have proven the output can hold them (see write_subframe()). The writer
// state lives in locals for the whole block, and full bytes are drained only
// when the next code does not fit. A code too long for the drained
// accumulator takes the checked path. The bits are identical to write_rice().
static FLAC_ALWAYS_INLINE void write_rice_block(BitWriterLocal& bw, const int32_t* residuals,
                                                uint32_t count, uint8_t k) {
    if (FLAC_UNLIKELY(bw.overflow)) {
        return;
    }

    bit_writer_acc_t acc = bw.acc;
    // Tracks free space rather than nbits, which saves two instructions per
    // residual on Xtensa
    uint32_t space = BIT_BUFFER_BITS - bw.nbits;
    uint8_t* pos = bw.pos;
    const uint32_t stop_bit = 1U << k;
    const uint32_t klow_mask = stop_bit - 1U;
    const uint32_t kp1 = static_cast<uint32_t>(k) + 1U;

    // A pointer against a limit, not an index: at Xtensa's register ceiling,
    // the extra live value would spill the loop invariants.
    const int32_t* const residuals_end = residuals + count;
    for (const int32_t* rp = residuals; rp != residuals_end; ++rp) {
        const int32_t r = *rp;
        // Zigzag, as in write_rice()
        const uint32_t u =
            // cppcheck-suppress shiftTooManyBitsSigned
            // cppcheck-suppress shiftNegativeLHS
            (static_cast<uint32_t>(r) << 1) ^ static_cast<uint32_t>(r >> 31);
        const uint32_t q = u >> k;

        // Two branches rather than one negated condition, which GCC
        // materializes as a value first. The first keeps q + kp1 from wrapping.
        if (FLAC_UNLIKELY(q >= space) || FLAC_UNLIKELY(q + kp1 > space)) {
            // Drain full bytes to maximize space, then retry once.
            while (space <= BIT_BUFFER_BITS - 8) {
                *pos++ = static_cast<uint8_t>(acc >> (BIT_BUFFER_BITS - 8));
                acc = static_cast<bit_writer_acc_t>(acc << 8);
                space += 8;
            }
            if (q >= space || q + kp1 > space) {
                // Long unary run: write this code through the checked path
                bw.acc = acc;
                bw.nbits = BIT_BUFFER_BITS - space;
                bw.pos = pos;
                write_zeros(bw, q);
                write_uint(bw, 1, 1);
                if (k > 0) {
                    write_uint(bw, u & klow_mask, k);
                }
                if (FLAC_UNLIKELY(bw.overflow)) {
                    return;
                }
                acc = bw.acc;
                space = BIT_BUFFER_BITS - bw.nbits;
                pos = bw.pos;
                continue;
            }
        }

        space -= q + kp1;
        acc |= static_cast<bit_writer_acc_t>(stop_bit | (u & klow_mask)) << space;
    }

    // Restore nbits < 8 and write the state back
    while (space <= BIT_BUFFER_BITS - 8) {
        *pos++ = static_cast<uint8_t>(acc >> (BIT_BUFFER_BITS - 8));
        acc = static_cast<bit_writer_acc_t>(acc << 8);
        space += 8;
    }
    bw.acc = acc;
    bw.nbits = BIT_BUFFER_BITS - space;
    bw.pos = pos;
}

// Pad with zero bits to the next byte boundary
static FLAC_ALWAYS_INLINE void write_align_zero(BitWriterLocal& bw) {
    if (FLAC_UNLIKELY(bw.overflow) || bw.nbits == 0) {
        return;
    }
    write_zeros(bw, 8 - bw.nbits);
}

// Store any partial byte, zero-padded, and return bw.pos - buffer_start, or 0
// on overflow. buffer_start may precede the pointer passed to writer_init():
// the encoder passes its frame's first byte, ahead of the header it writes
// directly.
static FLAC_ALWAYS_INLINE size_t writer_flush(BitWriterLocal& bw, const uint8_t* buffer_start) {
    if (bw.nbits != 0 && !bw.overflow) {
        if (FLAC_UNLIKELY(bw.pos >= bw.end)) {
            bw.overflow = true;
        } else {
            *bw.pos++ = static_cast<uint8_t>(bw.acc >> (BIT_BUFFER_BITS - 8));
        }
        bw.acc = 0;
        bw.nbits = 0;
    }

    if (FLAC_UNLIKELY(bw.overflow)) {
        return 0;
    }
    return static_cast<size_t>(bw.pos - buffer_start);
}

}  // namespace micro_flac
