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

/// @file subframe_coding.h
/// @brief What the encoder's subframe coders share
///
/// The signal a subframe codes (PackedSignal) and Pass A's verdict on it
/// (ChannelAnalysis), the fixed predictors' difference cascade, Rice code
/// estimation and emission, Pass B's size bound and the subframe header
/// writers. flac_encoder.cpp runs the passes over them, and
/// rice_partitions.cpp splits a subframe's residuals into partitions.

#pragma once

#include "bit_writer.h"
#include "compiler.h"
#include "flac_format.h"
#include "pcm_packing.h"
#include "wrapping_arithmetic.h"

#include <cstddef>
#include <cstdint>

#ifdef MICRO_FLAC_CHECK_SCAN_SUMS
#include <cstdlib>
#endif

namespace micro_flac {

// Deepest stream Pass A scans in 32 bits: an order-o residual is bounded by
// 2^(bps + o - 1), so the widest subframe, 24-bit stereo's 25-bit side at
// order 4, stays below 2^28 and its zigzag below 2^29. Deeper streams take
// scan64().
constexpr uint32_t MAX_SCAN32_BITS_PER_SAMPLE = 24;

// ============================================================================
// Rice coding
// ============================================================================

// Zigzag-map a signed residual to unsigned, as write_rice() does. The
// arithmetic right shift is implementation-defined before C++20; every target
// here sign-extends.
FLAC_ALWAYS_INLINE uint32_t zigzag(int32_t r) {
    const uint32_t low_bit = static_cast<uint32_t>(r) << 1;
    const uint32_t sign_extended =
        // cppcheck-suppress shiftTooManyBitsSigned
        static_cast<uint32_t>(r >> 31);  // NOLINT(readability-magic-numbers)
    return low_bit ^ sign_extended;
}

// Bits per Rice parameter for a subframe whose largest is max_k: 4 (coding
// method 0) while they all fit, else 5 (method 1).
FLAC_ALWAYS_INLINE uint32_t rice_parameter_bits(uint32_t max_k) {
    return (max_k > RICE_PARAMETER_MAX_4BIT) ? RICE_PARAMETER_BITS + 1 : RICE_PARAMETER_BITS;
}

// The residual coding method for `parameter_bits`-bit parameters (RFC 9639 SS9.2.7)
FLAC_ALWAYS_INLINE void write_coding_method(BitWriterLocal& bw, uint32_t parameter_bits) {
    write_uint(bw, parameter_bits - RICE_PARAMETER_BITS, RESIDUAL_CODING_METHOD_BITS);
}

// Size of an n-sample VERBATIM subframe at depth bps (wasted bits aside), the
// ceiling every other coding must come in under
FLAC_ALWAYS_INLINE uint64_t verbatim_subframe_bits(uint32_t n, uint32_t bps) {
    return SUBFRAME_HEADER_BITS + (static_cast<uint64_t>(n) * bps);
}

// Estimated Rice code bits for `count` residuals whose magnitudes sum to
// `abs_sum`, at the cheaper of floor(log2(mean)) and the parameter above it
// (a geometric distribution's optimum lies between them), which goes to k_out.
//
// Each code is k + 1 bits plus its quotient. Zigzag maps magnitude m to 2m or
// 2m - 1, so the quotients sum to about (2 * abs_sum) >> k, less about
// count / 2 for the bits the shift drops (as in libFLAC's estimator).
// count * (k + 1) alone exceeds that correction, so it cannot underflow.
//
// Callers use the 32-bit overload below whenever the sum fits, which on
// Xtensa is several times cheaper.
uint64_t estimate_rice_bits(uint64_t abs_sum, uint32_t count, uint8_t& k_out);

// The same estimate in 32-bit arithmetic. Nothing overflows: count < 2^16 and
// k <= 30 keep the k + 1 bits under 2^21, and the quotient term, written as
// abs_sum >> (k - 1), is under 4 * count (8 when k is capped). At k = 0 the
// sum is under 2 * count, so doubling it is safe. With m = msb(abs_sum) -
// msb(count), count << m stays below 2^(msb(abs_sum) + 1) <= 2^32.
FLAC_ALWAYS_INLINE uint32_t estimate_rice_bits(uint32_t abs_sum, uint32_t count, uint8_t& k_out) {
    uint32_t k0 = 0;
    if (abs_sum != 0) {
        const int m = (31 - __builtin_clz(abs_sum)) - (31 - __builtin_clz(count));
        if (m > RICE_PARAMETER_MAX) {
            k0 = RICE_PARAMETER_MAX;
        } else if (m > 0) {
            k0 =
                ((count << m) <= abs_sum) ? static_cast<uint32_t>(m) : static_cast<uint32_t>(m - 1);
        }
    }
    const uint32_t rounding = count >> 1;
    const uint32_t quotients0 = (k0 == 0) ? (abs_sum << 1) : (abs_sum >> (k0 - 1U));
    const uint32_t bits0 = count * (k0 + 1U) + quotients0 - rounding;
    k_out = static_cast<uint8_t>(k0);
    if (k0 < RICE_PARAMETER_MAX) {
        const uint32_t bits1 = count * (k0 + 2U) + (abs_sum >> k0) - rounding;
        if (bits1 < bits0) {
            k_out = static_cast<uint8_t>(k0 + 1U);
            return bits1;
        }
    }
    return bits0;
}

// estimate_rice_bits() in 32-bit arithmetic when the sum allows
FLAC_ALWAYS_INLINE uint64_t estimate_rice(uint64_t abs_sum, uint32_t count, uint8_t& k) {
    return ((abs_sum >> 32) == 0) ? estimate_rice_bits(static_cast<uint32_t>(abs_sum), count, k)
                                  : estimate_rice_bits(abs_sum, count, k);
}

// ============================================================================
// Signals and their analysis
// ============================================================================

// A signal a subframe can carry. Mono uses only LEFT; stereo analyzes all
// four and writes the two its channel assignment needs.
enum class SignalId : uint8_t { LEFT, RIGHT, MID, SIDE };

// Pass A's verdict on one signal. Largest members first, so it packs to 16
// bytes: it is copied through the stereo planner on the hot function's stack.
struct ChannelAnalysis {
    uint64_t estimated_bits{0};  // Estimated subframe size, capped at VERBATIM's
                                 // (see fixed_subframe_bits_bound())
    bool is_constant{false};
    bool verbatim{false};  // No fixed order keeps RFC 9639 SS9.2.7.3's residual
                           // limit (25-32 bit streams only)
    uint8_t order{0};      // Fixed-predictor order (0-4), unless is_constant or verbatim
    uint8_t k{0};          // Rice parameter for `order`
};

// |v| as uint32, defined for INT32_MIN. Written as a compare-and-select
// because GCC turns it into Xtensa's single ABS instruction; the branchless
// (v ^ (v >> 31)) - (v >> 31) form costs three instructions, and scan_range()
// has no registers to spare for them.
FLAC_ALWAYS_INLINE uint32_t abs_u32(int32_t v) {
    return (v < 0) ? (0U - static_cast<uint32_t>(v)) : static_cast<uint32_t>(v);
}

// Most samples whose uint32_t partial sums cannot overflow at subframe depth
// bps. Every |e_o| (o <= 4) is below 2^(bps + 3), so C of them fit in 32 bits
// when C <= 2^(29 - bps): 1024 up to 19 bits (16-bit stereo's 17-bit side
// included), 16 for 24-bit stereo's 25-bit side.
//
// An overflow would rarely change the output, but it would break
// fixed_subframe_bits_bound(), which get_max_output_bytes() relies on, so builds
// defining MICRO_FLAC_CHECK_SCAN_SUMS widen the partials and trap on one.
constexpr uint32_t MAX_SUM_CHUNK_SAMPLES = 1024;
FLAC_ALWAYS_INLINE uint32_t sum_chunk_samples(uint32_t bps) {
    // NOLINTNEXTLINE(readability-magic-numbers) -- C <= 2^(29 - bps), as above
    return (bps <= 19) ? MAX_SUM_CHUNK_SAMPLES : (1U << (29 - bps));
}

#ifdef MICRO_FLAC_CHECK_SCAN_SUMS
using ScanPartial = uint64_t;
inline void check_scan_partial(uint64_t partial) {
    if (partial > UINT32_MAX) {
        std::abort();
    }
}
#else
using ScanPartial = uint32_t;
FLAC_ALWAYS_INLINE void check_scan_partial(uint32_t /*partial*/) {}
#endif

// ============================================================================
// Reading a signal's residuals
// ============================================================================

// Pass B computes fixed residuals with Pass A's difference cascade, in place
// in each unpacked chunk, which the Rice writer then reads. Fusing the
// cascade with the bit writer's state would exceed Xtensa's registers.

// The cascade's state: the previous sample and its first three differences
struct Cascade {
    int32_t prev_s;
    int32_t prev_e1;
    int32_t prev_e2;
    int32_t prev_e3;
};

// Consume sample s0 and return its order-ORDER residual. The levels above
// ORDER are dead, so this folds to ORDER subtractions. They wrap: past 24
// bits the lower orders may overflow (and a 33-bit side arrives wrapped), but
// the residual comes out exact, since Pass A picks only orders that fit.
template <uint8_t ORDER>
FLAC_ALWAYS_INLINE int32_t cascade_step(int32_t s0, Cascade& c) {
    const int32_t e1 = wsub32(s0, c.prev_s);
    const int32_t e2 = wsub32(e1, c.prev_e1);
    const int32_t e3 = wsub32(e2, c.prev_e2);
    const int32_t e4 = wsub32(e3, c.prev_e3);
    c.prev_e3 = e3;
    c.prev_e2 = e2;
    c.prev_e1 = e1;
    c.prev_s = s0;
    return (ORDER == 0) ? s0 : (ORDER == 1) ? e1 : (ORDER == 2) ? e2 : (ORDER == 3) ? e3 : e4;
}

// Cascade state for the first order-ORDER residual, at index ORDER: running
// the cascade over the warmup samples leaves exactly the slots ORDER reads
// seeded.
template <uint8_t ORDER>
FLAC_ALWAYS_INLINE Cascade seed_cascade(const int32_t* samples) {
    Cascade c{0, 0, 0, 0};
    for (uint32_t i = 0; i < ORDER; i++) {
        cascade_step<ORDER>(samples[i], c);
    }
    return c;
}

// One channel of the packed input, or one of the four stereo candidates. Each
// chunk is unpacked into chunk_ and turned into residuals in place.
// Wasted bits are shifted out as it is read: a channel is unpacked as a
// shallower left-justified depth, and a derived candidate is shifted after
// its derivation.
struct PackedSignal {
    const uint8_t* input;
    uint32_t n;
    uint32_t bps;    // The stream's depth, which the input is packed at
    uint32_t bytes;  // Bytes per packed sample (1-4)
    uint32_t num_channels;
    uint32_t channel;  // The channel read when num_channels != 2
    SignalId sig;      // The candidate derived when num_channels == 2
    int32_t* buf;      // chunk_; unpack() fills its first chunk_len samples, and the
                       // stereo search's joint walk unpacks right into the second
    uint32_t chunk_len;
    uint32_t wasted;  // Low zero bits shifted out of every sample

    FLAC_ALWAYS_INLINE uint32_t wasted_bits() const {
        return this->wasted;
    }

    // Unpack samples [start, start + m) into chunk_ and return them. Defined
    // here so the linker keeps flac_encoder.cpp's copy beside its callers:
    // moved to subframe_coding.cpp, it cost 24-bit encodes 0.2-0.3% on the
    // ESP32-S3 (instruction-cache placement).
    FLAC_NOINLINE int32_t* unpack(uint32_t start, uint32_t m) const {
        if (FLAC_UNLIKELY(this->bytes == 4)) {
            return this->unpack32(start, m);
        }
        const size_t frame_bytes = static_cast<size_t>(this->bytes) * this->num_channels;
        const uint8_t* src = this->input + (start * frame_bytes);
        const uint32_t shift = this->wasted;
        if (this->num_channels != 2) {
            unpack_channel(this->buf, src, m, this->channel, this->num_channels, this->bps - shift,
                           this->bytes);
            return this->buf;
        }
        // Mid and side use left_right_to_mid_side()'s formulas.
        switch (this->sig) {
            case SignalId::LEFT:
            case SignalId::RIGHT: {
                const uint32_t ch = (this->sig == SignalId::RIGHT) ? 1 : 0;
                unpack_channel(this->buf, src, m, ch, 2, this->bps - shift, this->bytes);
                return this->buf;
            }
            case SignalId::MID:
                unpack_mid(this->buf, src, m, this->bps, this->bytes, shift);
                return this->buf;
            default:  // SIDE
                unpack_side(this->buf, src, m, this->bps, this->bytes, shift);
                return this->buf;
        }
    }

    // unpack() for 4-byte samples
    int32_t* unpack32(uint32_t start, uint32_t m) const;

    // Sample i, in 64 bits for a 33-bit side
    int64_t warmup_sample(uint32_t i) const {
        const size_t frame_bytes = static_cast<size_t>(this->bytes) * this->num_channels;
        const uint8_t* frame = this->input + (i * frame_bytes);
        if (this->num_channels != 2) {
            return unpack_sample(frame + (static_cast<size_t>(this->bytes) * this->channel),
                                 this->bps - this->wasted, this->bytes);
        }
        const int64_t l = unpack_sample(frame, this->bps, this->bytes);
        const int64_t r = unpack_sample(frame + this->bytes, this->bps, this->bytes);
        switch (this->sig) {
            case SignalId::RIGHT:
                return r >> this->wasted;
            case SignalId::MID:
                return (l + r) >> (1 + this->wasted);
            case SignalId::SIDE:
                return (l - r) >> this->wasted;
            default:  // LEFT
                return l >> this->wasted;
        }
    }

    template <uint8_t ORDER, typename Sink>
    void residual_chunks(Sink& sink) const {
        Cascade c{0, 0, 0, 0};
        for (uint32_t start = 0; start < this->n; start += this->chunk_len) {
            const uint32_t m =
                (this->n - start > this->chunk_len) ? this->chunk_len : (this->n - start);
            int32_t* s = this->unpack(start, m);
            // The first chunk holds the warmup samples, which seed the
            // cascade (it is chunk_len >= 4 long, or all n >= ORDER samples).
            uint32_t j = 0;
            if (start == 0) {
                c = seed_cascade<ORDER>(s);
                j = ORDER;
            }
            if (ORDER != 0) {
                for (uint32_t t = j; t < m; t++) {
                    s[t] = cascade_step<ORDER>(s[t], c);
                }
            }
            if (m > j) {
                sink(s + j, m - j);
            }
        }
    }
};

// Hand `signal`'s order-`order` residuals to `sink`, chunk by chunk.
template <typename Sink>
FLAC_ALWAYS_INLINE void for_each_residual_chunk(const PackedSignal& signal, uint8_t order,
                                                Sink&& sink) {
    switch (order) {
        case 0:
            signal.residual_chunks<0>(sink);
            break;
        case 1:
            signal.residual_chunks<1>(sink);
            break;
        case 2:
            signal.residual_chunks<2>(sink);
            break;
        case 3:
            signal.residual_chunks<3>(sink);
            break;
        default:  // 4
            signal.residual_chunks<4>(sink);
            break;
    }
}

// ============================================================================
// Writing subframes
// ============================================================================

// ----------------------------------------------------------------------------
// Pass B's size bound
// ----------------------------------------------------------------------------
//
// Pass B writes FIXED at Pass A's Rice parameter, deciding FIXED against
// VERBATIM and proving output capacity from Pass A's sums alone. That rests
// on a hard bound: a residual costs (zigzag(r) >> k) + k + 1 bits and
// zigzag(r) <= 2|r|, so c residuals with magnitude sum S cost at most
// c * (k + 1) + ((2S) >> k). That is estimate_rice_bits() before its c/2
// rounding correction, so the exact size is at most estimated_bits + c / 2.
// When that fails to prove FIXED smaller than VERBATIM (near-full-scale
// noise, or a capped estimate), the exact size decides.
//
// That bound for any subframe estimated this way, `order` warmup samples
// ahead of its residuals
FLAC_ALWAYS_INLINE uint64_t residual_bits_bound(uint64_t estimated_bits, uint32_t n,
                                                uint32_t order) {
    return estimated_bits + ((n - order) >> 1);
}

// ----------------------------------------------------------------------------
// Rice codes and subframe headers
// ----------------------------------------------------------------------------

// Whether `bits` more bits fit the remaining output, which lets the codes go
// through the unchecked write_rice_block()
FLAC_ALWAYS_INLINE bool capacity_for(const BitWriterLocal& bw, uint64_t bits) {
    const uint64_t bytes = (bw.nbits + bits + 7U) / 8U;
    return !bw.overflow && bytes <= static_cast<uint64_t>(bw.end - bw.pos);
}

// Write `count` Rice codes, unchecked when capacity is proven, else checked per code
FLAC_ALWAYS_INLINE void emit_rice_codes(BitWriterLocal& bw, const int32_t* residuals,
                                        uint32_t count, uint8_t k, bool capacity_proven) {
    if (FLAC_LIKELY(capacity_proven)) {
        write_rice_block(bw, residuals, count, k);
    } else {
        for (uint32_t i = 0; i < count; i++) {
            write_rice(bw, residuals[i], k);
        }
    }
}

// Subframe header (RFC 9639 SS9.2.1, SS9.2.2): the type, the wasted-bits
// flag, and wasted - 1 in unary. The unary bits are the same for every
// coding of a signal, so the size estimates leave them out and only the
// capacity checks add them. The k * n bits wasted bits save cover them, so
// get_max_output_bytes() still holds.
FLAC_ALWAYS_INLINE void write_subframe_header(BitWriterLocal& bw, uint8_t type, uint32_t wasted) {
    write_uint(bw, (static_cast<uint32_t>(type) << 1) | ((wasted != 0) ? 1U : 0U),
               SUBFRAME_HEADER_BITS);
    if (wasted != 0) {
        write_uint(bw, 1, static_cast<uint8_t>(wasted));  // wasted - 1 zeros, then a one
    }
}

// Write one sample bps8 (up to 33) bits deep
FLAC_ALWAYS_INLINE void write_sample(BitWriterLocal& bw, int64_t sample, uint8_t bps8) {
    const uint64_t bits = static_cast<uint64_t>(sample);
    if (FLAC_UNLIKELY(bps8 > 32)) {
        write_uint(bw, static_cast<uint32_t>(bits >> 32), static_cast<uint8_t>(bps8 - 32));
        bps8 = 32;
    }
    write_uint(bw, static_cast<uint32_t>(bits), bps8);
}

// Write the first `order` samples, a FIXED subframe's warmup
void write_warmup(BitWriterLocal& bw, const PackedSignal& signal, uint32_t order, uint8_t bps8);

// FIXED subframe header (RFC 9639 SS9.2.5) through the first Rice parameter
FLAC_ALWAYS_INLINE void write_fixed_header(BitWriterLocal& bw, const PackedSignal& signal,
                                           uint8_t bps8, uint8_t order, uint8_t partition_order,
                                           uint8_t k, uint32_t parameter_bits) {
    write_subframe_header(bw, static_cast<uint8_t>(SUBFRAME_TYPE_FIXED_MIN + order),
                          signal.wasted_bits());
    write_warmup(bw, signal, order, bps8);
    write_coding_method(bw, parameter_bits);
    write_uint(bw, partition_order, RICE_PARTITION_ORDER_BITS);
    write_uint(bw, k, static_cast<uint8_t>(parameter_bits));
}

}  // namespace micro_flac
