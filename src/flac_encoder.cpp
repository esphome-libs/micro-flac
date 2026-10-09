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

#include "micro_flac/flac_encoder.h"

#include "bit_writer.h"
#include "compiler.h"
#include "crc.h"
#include "flac_format.h"
#include "frame_header.h"
#include "pcm_packing.h"

#include <cstdint>
#include <cstring>

#ifdef MICRO_FLAC_CHECK_SCAN_SUMS
#include <cstdlib>
#endif

// Unpacking (pcm_packing.cpp) reads the little-endian input's 2-byte samples
// as native int16_t and its 3-byte samples as little-endian words.
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "FLACEncoder requires a little-endian target"
#endif

namespace micro_flac {

namespace {

// ============================================================================
// Constants
// ============================================================================

// "fLaC", the STREAMINFO block header (4 bytes), then its body
static_assert(FLACEncoder::HEADER_BYTES == sizeof(MAGIC_BYTES) + 4 + STREAMINFO_SIZE,
              "FLACEncoder::HEADER_BYTES must match the header write_header() emits");

// Deepest supported stream (the shallowest is the format's, MIN_BITS_PER_SAMPLE).
// 24 keeps every fixed residual in int32_t: an order-o residual is bounded by
// 2^(bps + o - 1), so the widest subframe, 24-bit stereo's 25-bit side at
// order 4, stays below 2^28 and its zigzag below 2^29.
constexpr uint32_t MAX_BITS_PER_SAMPLE = 24;

// ============================================================================
// Rice parameter estimation (Pass A helper)
// ============================================================================

// floor(log2(sum / n)), capped at RICE_PARAMETER_MAX: the largest k with
// (n << k) <= sum, or 0. msb(sum) - msb(n) narrows it to two values and one
// comparison picks, avoiding a 64-bit divide (a libcall on Xtensa).
uint8_t estimate_rice_k(uint64_t sum, uint32_t n) {
    if (sum == 0 || n == 0) {
        return 0;
    }
    const int msb_sum = 63 - __builtin_clzll(sum);
    const int msb_n = 31 - __builtin_clz(n);
    const int m = msb_sum - msb_n;  // floor(log2(sum / n)) is m or m - 1
    if (m <= 0) {
        return 0;  // sum / n < 2, so k = 0 (covers m == 0's both candidates)
    }
    if (m > RICE_PARAMETER_MAX) {
        return RICE_PARAMETER_MAX;  // even m - 1 >= the cap, no need to resolve
    }
    // m <= 30 and msb(n) + m <= msb(sum) <= 63, so the shift cannot overflow.
    return (static_cast<uint64_t>(n) << m) <= sum ? static_cast<uint8_t>(m)
                                                  : static_cast<uint8_t>(m - 1);
}

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

// ============================================================================
// Rice cost estimation (Pass A helper)
// ============================================================================

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
uint64_t estimate_rice_bits(uint64_t abs_sum, uint32_t count, uint8_t& k_out) {
    const uint8_t k0 = estimate_rice_k(abs_sum, count);
    const uint64_t doubled = abs_sum << 1;
    const uint64_t rounding = count >> 1;
    const uint64_t bits0 = static_cast<uint64_t>(count) * (k0 + 1U) + (doubled >> k0) - rounding;
    k_out = k0;
    if (k0 < RICE_PARAMETER_MAX) {
        const uint64_t bits1 =
            static_cast<uint64_t>(count) * (k0 + 2U) + (doubled >> (k0 + 1U)) - rounding;
        if (bits1 < bits0) {
            k_out = static_cast<uint8_t>(k0 + 1U);
            return bits1;
        }
    }
    return bits0;
}

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
// Signals and their samples
// ============================================================================

// A signal a subframe can carry. Mono uses only LEFT; stereo analyzes all
// four and writes the two its channel assignment needs.
enum class SignalId : uint8_t { LEFT, RIGHT, MID, SIDE };

// ============================================================================
// Chunked unpacking
// ============================================================================

// Every format is unpacked into int32_t chunks in FLACEncoder::chunk_ and
// analyzed and written from those, so one instantiation of the per-order
// machinery serves them all, with no block-sized buffer. The cost is
// unpacking the input again for each walk over a subframe.

// Convert unpacked left/right to mid/side in place: mid = (L + R) >> 1 and
// side = L - R. The arithmetic shift matches the decoder's reconstruction
// (apply_channel_decorrelation()).
void left_right_to_mid_side(int32_t* left_to_mid, int32_t* right_to_side, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        const int32_t l = left_to_mid[i];
        const int32_t r = right_to_side[i];
        left_to_mid[i] = (l + r) >> 1;
        right_to_side[i] = l - r;
    }
}

// left_right_to_mid_side(), also ORing each candidate's samples into `bits`
// (indexed by SignalId) for wasted-bits detection while all four are in
// registers.
FLAC_NOINLINE void left_right_to_mid_side_or(int32_t* left_to_mid, int32_t* right_to_side,
                                             uint32_t n, uint32_t (&bits)[4]) {
    uint32_t bits_l = 0;
    uint32_t bits_r = 0;
    uint32_t bits_mid = 0;
    uint32_t bits_side = 0;
    for (uint32_t i = 0; i < n; i++) {
        const int32_t l = left_to_mid[i];
        const int32_t r = right_to_side[i];
        const int32_t mid = (l + r) >> 1;
        const int32_t side = l - r;
        bits_l |= static_cast<uint32_t>(l);
        bits_r |= static_cast<uint32_t>(r);
        bits_mid |= static_cast<uint32_t>(mid);
        bits_side |= static_cast<uint32_t>(side);
        left_to_mid[i] = mid;
        right_to_side[i] = side;
    }
    bits[0] |= bits_l;
    bits[1] |= bits_r;
    bits[2] |= bits_mid;
    bits[3] |= bits_side;
}

// ============================================================================
// Pass A: per-signal cost analysis
// ============================================================================

// Pass A's verdict on one signal. Largest members first, so it packs to 16
// bytes: it is copied through the stereo planner on the hot function's stack.
struct ChannelAnalysis {
    uint64_t estimated_bits{0};  // Estimated subframe size, capped at VERBATIM's
                                 // (see fixed_subframe_bits_bound())
    int32_t constant_value{0};
    bool is_constant{false};
    uint8_t order{0};  // Fixed-predictor order (0-4), unless is_constant
    uint8_t k{0};      // Rice parameter for `order`
};

// |v| as uint32, defined for INT32_MIN. Written as a compare-and-select
// because GCC turns it into Xtensa's single ABS instruction; the branchless
// (v ^ (v >> 31)) - (v >> 31) form costs three instructions, and scan_range()
// has no registers to spare for them.
FLAC_ALWAYS_INLINE uint32_t abs_u32(int32_t v) {
    return (v < 0) ? (0U - static_cast<uint32_t>(v)) : static_cast<uint32_t>(v);
}

// Pass A's state for one signal, carried across calls so a signal scanned
// chunk by chunk gets the same result as one whole-block scan.
//
// sum[o] is the sum of |e_o[i]| over i >= o, where e_0 is the sample and the
// order-o fixed residual e_o is the o-th difference: e_1[i] = s[i] - s[i-1],
// e_2[i] = e_1[i] - e_1[i-1], and so on. The cascade gives the same values as
// the fixed-predictor polynomials with one subtraction per order and no
// multiplies. prev[o] holds the previous sample's e_o.
struct SignalScan {
    uint64_t sum[MAX_FIXED_ORDER + 1];
    int32_t prev[MAX_FIXED_ORDER];
    int32_t first;  // Sample 0, a CONSTANT subframe's value
};

// Samples before every order has a residual (order o's first is at i == o)
FLAC_ALWAYS_INLINE uint32_t scan_warmup_samples(uint32_t n) {
    return (n < MAX_FIXED_ORDER) ? n : MAX_FIXED_ORDER;
}

// Scan the warmup samples, peeled out so scan_range()'s loop has no branches
FLAC_ALWAYS_INLINE void scan_begin(SignalScan& scan, const int32_t* samples, uint32_t warmup) {
    for (uint64_t& total : scan.sum) {
        total = 0;
    }
    int32_t prev[MAX_FIXED_ORDER] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < warmup; i++) {
        const int32_t s0 = samples[i];
        const int32_t e1 = s0 - prev[0];
        const int32_t e2 = e1 - prev[1];
        const int32_t e3 = e2 - prev[2];
        scan.sum[0] += abs_u32(s0);
        if (i >= 1) {
            scan.sum[1] += abs_u32(e1);
        }
        if (i >= 2) {
            scan.sum[2] += abs_u32(e2);
        }
        if (i >= 3) {
            scan.sum[3] += abs_u32(e3);
        }
        prev[3] = e3;
        prev[2] = e2;
        prev[1] = e1;
        prev[0] = s0;
    }
    for (uint32_t o = 0; o < MAX_FIXED_ORDER; o++) {
        scan.prev[o] = prev[o];
    }
    scan.first = (warmup != 0) ? samples[0] : 0;
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

// Continue a scan over samples [i, end), past the warmup, carrying all five
// orders in one loop. Its 11 live values fit Xtensa's 14 usable registers
// only because abs_u32() is one instruction. Anything added to the loop
// (another reduction, 2x unrolling, 64-bit accumulators) spills every
// accumulator on every iteration; splitting the orders across two scans is
// the fallback. A constant signal is detected from sum[1] == 0 instead of a
// reduction of its own for this reason.
//
// Partial sums are uint32_t, added to the 64-bit totals every sum_chunk
// samples (sum_chunk_samples()).
FLAC_ALWAYS_INLINE void scan_range(SignalScan& scan, const int32_t* samples, uint32_t i,
                                   uint32_t end, uint32_t sum_chunk) {
    int32_t prev_s = scan.prev[0];
    int32_t prev_e1 = scan.prev[1];
    int32_t prev_e2 = scan.prev[2];
    int32_t prev_e3 = scan.prev[3];
    while (i < end) {
        const uint32_t chunk_end = (end - i > sum_chunk) ? (i + sum_chunk) : end;
        ScanPartial c0 = 0;
        ScanPartial c1 = 0;
        ScanPartial c2 = 0;
        ScanPartial c3 = 0;
        ScanPartial c4 = 0;
        for (; i < chunk_end; i++) {
            const int32_t s0 = samples[i];
            const int32_t e1 = s0 - prev_s;
            const int32_t e2 = e1 - prev_e1;
            const int32_t e3 = e2 - prev_e2;
            const int32_t e4 = e3 - prev_e3;
            c0 += abs_u32(s0);
            c1 += abs_u32(e1);
            c2 += abs_u32(e2);
            c3 += abs_u32(e3);
            c4 += abs_u32(e4);
            prev_e3 = e3;
            prev_e2 = e2;
            prev_e1 = e1;
            prev_s = s0;
        }
        check_scan_partial(c0);
        check_scan_partial(c1);
        check_scan_partial(c2);
        check_scan_partial(c3);
        check_scan_partial(c4);
        scan.sum[0] += c0;
        scan.sum[1] += c1;
        scan.sum[2] += c2;
        scan.sum[3] += c3;
        scan.sum[4] += c4;
    }
    scan.prev[0] = prev_s;
    scan.prev[1] = prev_e1;
    scan.prev[2] = prev_e2;
    scan.prev[3] = prev_e3;
}

// Pick the fixed-predictor order and Rice parameter with the lowest
// estimated size from a finished scan, at subframe depth bps (one more than
// the stream's for a side signal). Out of line: it runs once per signal,
// off the hot path.
FLAC_NOINLINE ChannelAnalysis finish_analysis(const SignalScan& scan, uint32_t n, uint32_t bps) {
    ChannelAnalysis result{};

    // Every first difference was zero <=> every sample equals the first.
    const bool constant = (scan.sum[1] == 0);

    result.is_constant = constant;
    result.constant_value = scan.first;
    if (constant) {
        result.estimated_bits = SUBFRAME_HEADER_BITS + bps;
        return result;
    }

    // n < MAX_FIXED_ORDER only when n < 4, so the cast to uint8_t is exact.
    const uint8_t max_order = (n < MAX_FIXED_ORDER) ? static_cast<uint8_t>(n) : MAX_FIXED_ORDER;

    uint64_t best_bits = UINT64_MAX;
    uint8_t best_order = 0;
    uint8_t best_k = 0;

    for (uint8_t order = 0; order <= max_order; order++) {
        const uint32_t residual_count = n - order;
        const uint64_t sum = scan.sum[order];
        // Subframe header, warmup samples, residual header (method, partition
        // order, a 4-bit parameter), then the codes. A 5-bit parameter's
        // extra bit is added once, for the chosen order, below; per order it
        // could only break a one-bit tie.
        uint8_t k = 0;
        const uint64_t est_bits = SUBFRAME_HEADER_BITS + static_cast<uint64_t>(order) * bps +
                                  RESIDUAL_CODING_METHOD_BITS + RICE_PARTITION_ORDER_BITS +
                                  RICE_PARAMETER_BITS + estimate_rice(sum, residual_count, k);
        if (est_bits < best_bits) {
            best_bits = est_bits;
            best_order = order;
            best_k = k;
        }
    }

    // Pass B never writes more than VERBATIM, so the estimate is capped there
    // too, or the stereo plan would overprice noise-like candidates. A capped
    // estimate sends Pass B down fixed_subframe_budget()'s exact path.
    best_bits += rice_parameter_bits(best_k) - RICE_PARAMETER_BITS;
    const uint64_t verbatim_bits = verbatim_subframe_bits(n, bps);
    result.order = best_order;
    result.k = best_k;
    result.estimated_bits = (best_bits < verbatim_bits) ? best_bits : verbatim_bits;
    return result;
}

// Continue a scan over [begin, end) of an unpacked chunk, out of line so its
// several call sites share one copy
FLAC_NOINLINE void scan_chunk(SignalScan& scan, const int32_t* chunk, uint32_t begin, uint32_t end,
                              uint32_t bps) {
    scan_range(scan, chunk, begin, end, sum_chunk_samples(bps));
}

// The OR of m samples, whose trailing zeros are their wasted bits. A loop of
// its own, since scan_range() has no register to spare.
FLAC_NOINLINE uint32_t or_samples(const int32_t* s, uint32_t m) {
    uint32_t bits = 0;
    for (uint32_t i = 0; i < m; i++) {
        bits |= static_cast<uint32_t>(s[i]);
    }
    return bits;
}

// finish_analysis() for a signal whose samples OR to `bits`: with k wasted
// bits, it is coded shifted right by k, at depth bps - k. Every residual of
// the shifted signal is the original's divided by 2^k, so the sums are
// rescaled rather than rescanned. `wasted` receives k; a constant signal gets
// none.
FLAC_NOINLINE ChannelAnalysis finish_wasted_analysis(SignalScan scan, uint32_t n, uint32_t bps,
                                                     uint32_t bits, uint32_t& wasted) {
    wasted = 0;
    if (bits != 0 && scan.sum[1] != 0) {
        wasted = static_cast<uint32_t>(__builtin_ctz(bits));
        for (uint64_t& sum : scan.sum) {
            sum >>= wasted;
        }
    }
    return finish_analysis(scan, n, bps - wasted);
}

// ============================================================================
// Pass A: joint-stereo assignment decision
// ============================================================================

// A stereo frame's channel assignment and its two subframes, in wire order
struct StereoPlan {
    uint8_t channel_assignment{0};
    SignalId sig0{SignalId::LEFT};
    SignalId sig1{SignalId::RIGHT};
    uint32_t bps0{0};
    uint32_t bps1{0};
    ChannelAnalysis analysis0;
    ChannelAnalysis analysis1;
};

// Pick the cheapest of independent, left/side, right/side and mid/side from
// the four candidates' estimates, ties going to the earlier. Side is coded
// one bit deeper than the stream's bps (RFC 9639 SS9.1.3).
StereoPlan choose_stereo_plan(const ChannelAnalysis& analysis_l, const ChannelAnalysis& analysis_r,
                              const ChannelAnalysis& analysis_mid,
                              const ChannelAnalysis& analysis_side, uint32_t bps) {
    const uint32_t side_bps = bps + 1;

    StereoPlan plan{};
    plan.channel_assignment = static_cast<uint8_t>(CHANNEL_INDEPENDENT_STEREO);
    plan.sig0 = SignalId::LEFT;
    plan.bps0 = bps;
    plan.analysis0 = analysis_l;
    plan.sig1 = SignalId::RIGHT;
    plan.bps1 = bps;
    plan.analysis1 = analysis_r;

    uint64_t best_cost = analysis_l.estimated_bits + analysis_r.estimated_bits;

    const uint64_t cost_left_side = analysis_l.estimated_bits + analysis_side.estimated_bits;
    if (cost_left_side < best_cost) {
        best_cost = cost_left_side;
        plan.channel_assignment = static_cast<uint8_t>(CHANNEL_LEFT_SIDE);
        plan.sig0 = SignalId::LEFT;
        plan.bps0 = bps;
        plan.analysis0 = analysis_l;
        plan.sig1 = SignalId::SIDE;
        plan.bps1 = side_bps;
        plan.analysis1 = analysis_side;
    }

    const uint64_t cost_right_side = analysis_side.estimated_bits + analysis_r.estimated_bits;
    if (cost_right_side < best_cost) {
        best_cost = cost_right_side;
        plan.channel_assignment = static_cast<uint8_t>(CHANNEL_RIGHT_SIDE);
        plan.sig0 = SignalId::SIDE;
        plan.bps0 = side_bps;
        plan.analysis0 = analysis_side;
        plan.sig1 = SignalId::RIGHT;
        plan.bps1 = bps;
        plan.analysis1 = analysis_r;
    }

    const uint64_t cost_mid_side = analysis_mid.estimated_bits + analysis_side.estimated_bits;
    if (cost_mid_side < best_cost) {
        plan.channel_assignment = static_cast<uint8_t>(CHANNEL_MID_SIDE);
        plan.sig0 = SignalId::MID;
        plan.bps0 = bps;
        plan.analysis0 = analysis_mid;
        plan.sig1 = SignalId::SIDE;
        plan.bps1 = side_bps;
        plan.analysis1 = analysis_side;
    }

    return plan;
}

// ============================================================================
// Pass B: subframe emission
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
// ORDER are dead, so this folds to ORDER subtractions.
template <uint8_t ORDER>
FLAC_ALWAYS_INLINE int32_t cascade_step(int32_t s0, Cascade& c) {
    const int32_t e1 = s0 - c.prev_s;
    const int32_t e2 = e1 - c.prev_e1;
    const int32_t e3 = e2 - c.prev_e2;
    const int32_t e4 = e3 - c.prev_e3;
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

// ----------------------------------------------------------------------------
// Reading a subframe's signal
// ----------------------------------------------------------------------------

// One channel of the packed input, or one of the four stereo candidates. Each
// chunk is unpacked into chunk_ and turned into residuals in place.
// Wasted bits are shifted out as it is read: a channel is unpacked as a
// shallower left-justified depth, and a derived candidate is shifted after
// its derivation.
struct PackedSignal {
    const uint8_t* input;
    uint32_t n;
    uint32_t bps;    // The stream's depth, which the input is packed at
    uint32_t bytes;  // Bytes per packed sample (1, 2 or 3)
    uint32_t num_channels;
    uint32_t channel;  // The channel read when num_channels != 2
    SignalId sig;      // The candidate derived when num_channels == 2
    int32_t* buf;      // chunk_; unpack() fills its first chunk_len samples
    uint32_t chunk_len;
    uint32_t wasted;  // Low zero bits shifted out of every sample

    FLAC_ALWAYS_INLINE uint32_t wasted_bits() const {
        return this->wasted;
    }

    // Unpack samples [start, start + m) into chunk_ and return them
    FLAC_NOINLINE int32_t* unpack(uint32_t start, uint32_t m) const {
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

    int32_t warmup_sample(uint32_t i) const {
        const size_t frame_bytes = static_cast<size_t>(this->bytes) * this->num_channels;
        const uint8_t* frame = this->input + (i * frame_bytes);
        if (this->num_channels != 2) {
            return unpack_sample(frame + (static_cast<size_t>(this->bytes) * this->channel),
                                 this->bps - this->wasted, this->bytes);
        }
        const int32_t l = unpack_sample(frame, this->bps, this->bytes);
        const int32_t r = unpack_sample(frame + this->bytes, this->bps, this->bytes);
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

// The bound for Pass A's unpartitioned FIXED subframe
FLAC_ALWAYS_INLINE uint64_t fixed_subframe_bits_bound(const ChannelAnalysis& analysis, uint32_t n) {
    return residual_bits_bound(analysis.estimated_bits, n, analysis.order);
}

// Exact size of the FIXED subframe, by another walk over its residuals. The
// rare path behind fixed_subframe_bits_bound(), so it stays out of line.
FLAC_NOINLINE uint64_t fixed_subframe_exact_bits(const PackedSignal& signal, uint32_t bps,
                                                 const ChannelAnalysis& analysis) {
    const uint8_t k = analysis.k;
    uint64_t quotients = 0;
    for_each_residual_chunk(signal, analysis.order, [&](const int32_t* r, uint32_t m) {
        for (uint32_t j = 0; j < m; j++) {
            quotients += zigzag(r[j]) >> k;
        }
    });
    const uint64_t residual_count = signal.n - analysis.order;
    return SUBFRAME_HEADER_BITS + static_cast<uint64_t>(analysis.order) * bps +
           RESIDUAL_CODING_METHOD_BITS + RICE_PARTITION_ORDER_BITS + rice_parameter_bits(k) +
           residual_count * (k + 1U) + quotients;
}

// An upper bound on the FIXED subframe's size when it is provably smaller than
// VERBATIM, else 0 (write VERBATIM). Never writing a FIXED subframe larger
// than VERBATIM is what makes get_max_output_bytes() a hard bound.
FLAC_ALWAYS_INLINE uint64_t fixed_subframe_budget(const PackedSignal& signal, uint32_t bps,
                                                  const ChannelAnalysis& analysis) {
    const uint64_t verbatim_total_bits = verbatim_subframe_bits(signal.n, bps);
    const uint64_t bound = fixed_subframe_bits_bound(analysis, signal.n);
    if (FLAC_LIKELY(bound < verbatim_total_bits)) {
        return bound;
    }
    const uint64_t exact = fixed_subframe_exact_bits(signal, bps, analysis);
    return (exact < verbatim_total_bits) ? exact : 0;
}

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

// VERBATIM subframe: the header, then every sample
FLAC_NOINLINE void write_verbatim_subframe(BitWriterLocal& bw, const PackedSignal& signal,
                                           uint8_t bps8) {
    write_subframe_header(bw, SUBFRAME_TYPE_VERBATIM, signal.wasted_bits());
    auto sink = [&](const int32_t* s, uint32_t m) {
        for (uint32_t i = 0; i < m; i++) {
            write_uint(bw, static_cast<uint32_t>(s[i]), bps8);
        }
    };
    signal.residual_chunks<0>(sink);
}

// FIXED subframe header (RFC 9639 SS9.2.5) through the first Rice parameter
FLAC_ALWAYS_INLINE void write_fixed_header(BitWriterLocal& bw, const PackedSignal& signal,
                                           uint8_t bps8, uint8_t order, uint8_t partition_order,
                                           uint8_t k, uint32_t parameter_bits) {
    write_subframe_header(bw, static_cast<uint8_t>(SUBFRAME_TYPE_FIXED_MIN + order),
                          signal.wasted_bits());
    for (uint32_t i = 0; i < order; i++) {
        write_uint(bw, static_cast<uint32_t>(signal.warmup_sample(i)), bps8);
    }
    write_coding_method(bw, parameter_bits);
    write_uint(bw, partition_order, RICE_PARTITION_ORDER_BITS);
    write_uint(bw, k, static_cast<uint8_t>(parameter_bits));
}

// ----------------------------------------------------------------------------
// Rice partitioning (FLACEncoderOptions::max_rice_partition_order)
// ----------------------------------------------------------------------------
//
// A subframe's residuals can be split into 2^p equal partitions, the first
// short by the predictor order, each with its own Rice parameter (RFC 9639
// SS9.2.7). It is chosen after the predictor, in Pass B: one walk sums the
// residual magnitudes per partition at the highest order allowed (the
// leaves), and choose_partitions() prices every order from those sums,
// merging neighbors bottom-up as libFLAC does. Pass A is unchanged. The size
// bound holds partition by partition, so estimate + (n - order) / 2 still
// bounds the subframe.
//
// A subframe that does not split takes the single-partition walks, so with
// partitioning off, every other option runs the same code.

#ifndef MICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS

constexpr uint32_t MAX_PARTITION_ORDER = FLACEncoder::MAX_RICE_PARTITION_ORDER;
constexpr uint32_t MAX_PARTITIONS = 1U << MAX_PARTITION_ORDER;

// A subframe's Rice coding: 2^order partitions and each one's parameter
struct RicePartitions {
    uint8_t order;
    uint8_t parameter_bits;  // 4, or 5 when any parameter exceeds RICE_PARAMETER_MAX_4BIT
    uint8_t k[MAX_PARTITIONS];
};

// Highest partition order up to max_order whose partition count divides n
// and whose first partition keeps a residual
FLAC_ALWAYS_INLINE uint32_t partition_order_limit(uint32_t n, uint32_t order, uint32_t max_order) {
    uint32_t p = 0;
    while (p < max_order && ((n >> (p + 1)) << (p + 1)) == n && (n >> (p + 1)) > order) {
        p++;
    }
    return p;
}

// Residual magnitude sums per leaf partition, filled by one walk. Positions
// are sample indices.
struct LeafSums {
    uint64_t sum[MAX_PARTITIONS];
    uint32_t len;    // Samples per leaf
    uint32_t end;    // Where the current leaf ends
    uint32_t pos;    // Where the walk stands
    uint32_t index;  // The current leaf
};

// Start leaf sums for an n-sample subframe split 2^p ways, walking from `pos`
FLAC_ALWAYS_INLINE void leaf_sums_begin(LeafSums& sums, uint32_t n, uint32_t p, uint32_t pos) {
    for (uint32_t i = 0; i < (1U << p); i++) {
        sums.sum[i] = 0;
    }
    sums.len = n >> p;
    sums.end = sums.len;
    sums.pos = pos;
    sums.index = 0;
}

// Advance to the next leaf at the current one's end, and return how many of
// the next m positions stay in the leaf
FLAC_ALWAYS_INLINE uint32_t leaf_step(LeafSums& sums, uint32_t m) {
    if (sums.pos == sums.end) {
        sums.index++;
        sums.end += sums.len;
    }
    const uint32_t left = sums.end - sums.pos;
    return (m < left) ? m : left;
}

// Choose the cheapest partitioning from leaf sums at order p, merging `sums`
// in place. Returns the estimated bits of the partition order field,
// parameters and codes; ties go to the lower order.
//
// Any parameter above 14 widens all of them to 5 bits, so when the widest is
// exactly 15, clamping those partitions to 14 is priced too.
FLAC_NOINLINE uint64_t choose_partitions(LeafSums& sums, uint32_t n, uint32_t order, uint32_t p,
                                         RicePartitions& out) {
    uint64_t best = UINT64_MAX;
    uint8_t k[MAX_PARTITIONS];
    for (;;) {
        const uint32_t parts = 1U << p;
        const uint32_t len = n >> p;
        uint64_t bits = RICE_PARTITION_ORDER_BITS;
        uint32_t max_k = 0;
        for (uint32_t i = 0; i < parts; i++) {
            const uint32_t count = (i == 0) ? (len - order) : len;
            // leaf_sums_begin() zeroed all 2^p leaves; the analyzer cannot
            // evaluate 1U << p and assumes none
            // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
            bits += estimate_rice(sums.sum[i], count, k[i]);
            max_k = (k[i] > max_k) ? k[i] : max_k;
        }
        uint32_t parameter_bits = rice_parameter_bits(max_k);
        if (max_k == RICE_PARAMETER_MAX_4BIT + 1) {
            uint64_t clamped = bits;
            for (uint32_t i = 0; i < parts; i++) {
                if (k[i] == max_k) {
                    const uint64_t count = (i == 0) ? (len - order) : len;
                    const uint64_t doubled = sums.sum[i] << 1;
                    // Both prices share the c/2 rounding correction
                    clamped += (count * max_k) + (doubled >> (max_k - 1U));
                    clamped -= (count * (max_k + 1U)) + (doubled >> max_k);
                }
            }
            if (clamped < bits + parts) {  // 4-bit parameters save a bit per partition
                bits = clamped;
                parameter_bits = RICE_PARAMETER_BITS;
                for (uint32_t i = 0; i < parts; i++) {
                    k[i] = (k[i] == max_k) ? static_cast<uint8_t>(max_k - 1U) : k[i];
                }
            }
        }
        bits += static_cast<uint64_t>(parts) * parameter_bits;
        if (bits <= best) {
            best = bits;
            out.order = static_cast<uint8_t>(p);
            out.parameter_bits = static_cast<uint8_t>(parameter_bits);
            for (uint32_t i = 0; i < parts; i++) {
                out.k[i] = k[i];
            }
        }
        if (p == 0) {
            return best;
        }
        for (size_t i = 0; i < parts / 2; i++) {
            sums.sum[i] = sums.sum[2 * i] + sums.sum[(2 * i) + 1];
        }
        p--;
    }
}

// Where a walk writing a subframe's Rice codes stands among its partitions
struct RiceEmitter {
    const RicePartitions* rice;
    uint32_t len;    // Samples per partition
    uint32_t end;    // Where the current partition ends
    uint32_t pos;    // Sample index of the next residual
    uint32_t index;  // The current partition
    bool capacity_proven;
};

// An emitter for a subframe whose header has written the first parameter
FLAC_ALWAYS_INLINE RiceEmitter rice_emitter(const RicePartitions& rice, uint32_t n, uint32_t order,
                                            bool capacity_proven) {
    const uint32_t len = n >> rice.order;
    return RiceEmitter{&rice, len, len, order, 0, capacity_proven};
}

// Write the next m residuals' codes, and each later partition's parameter
// where it starts
FLAC_NOINLINE void emit_partitioned(BitWriterLocal& bw, RiceEmitter& e, const int32_t* r,
                                    uint32_t m) {
    while (m != 0) {
        if (e.pos == e.end) {
            e.index++;
            e.end += e.len;
            write_uint(bw, e.rice->k[e.index], e.rice->parameter_bits);
        }
        const uint32_t left = e.end - e.pos;
        const uint32_t take = (m < left) ? m : left;
        emit_rice_codes(bw, r, take, e.rice->k[e.index], e.capacity_proven);
        r += take;
        m -= take;
        e.pos += take;
    }
}

// One sink type for the walks below, so they share their signal's
// residual_chunks() instantiations instead of each adding five
struct ResidualSink {
    void (*fn)(void* context, const int32_t* r, uint32_t m);
    void* context;

    void operator()(const int32_t* r, uint32_t m) const {
        this->fn(this->context, r, m);
    }
};

// Add the next m residuals to their leaves' sums, in uint32_t partials of at
// most sum_chunk_samples(bps)
FLAC_NOINLINE void add_leaf_residuals(LeafSums& sums, const int32_t* r, uint32_t m, uint32_t bps) {
    const uint32_t run_max = sum_chunk_samples(bps);
    while (m != 0) {
        uint32_t take = leaf_step(sums, m);
        take = (take < run_max) ? take : run_max;
        ScanPartial partial = 0;
        for (uint32_t j = 0; j < take; j++) {
            partial += abs_u32(r[j]);
        }
        check_scan_partial(partial);
        sums.sum[sums.index] += partial;
        r += take;
        m -= take;
        sums.pos += take;
    }
}

// Leave the FIXED subframe unpartitioned, at Pass A's parameter and estimate
FLAC_ALWAYS_INLINE uint64_t unpartitioned_fixed(const ChannelAnalysis& analysis,
                                                RicePartitions& rice) {
    rice.order = 0;
    rice.parameter_bits = static_cast<uint8_t>(rice_parameter_bits(analysis.k));
    rice.k[0] = analysis.k;
    return analysis.estimated_bits;
}

// The partitioning of the FIXED subframe at Pass A's order that Pass B will
// write, and its estimated size. A split is kept only when its bound proves
// it smaller than VERBATIM (as write_partitioned_fixed() needs); a dropped
// split, or a subframe too short to split, leaves analysis.estimated_bits.
// Out of line so the leaf sums take stack only here.
FLAC_NOINLINE uint64_t partition_fixed(const PackedSignal& signal, uint32_t bps,
                                       const ChannelAnalysis& analysis, uint32_t max_order,
                                       RicePartitions& rice) {
    const uint32_t n = signal.n;
    const uint32_t order = analysis.order;
    const uint32_t p = partition_order_limit(n, order, max_order);
    if (p == 0) {
        return unpartitioned_fixed(analysis, rice);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init) -- leaf_sums_begin() sets it
    struct Context {
        LeafSums sums;
        uint32_t bps;
    } context;
    leaf_sums_begin(context.sums, n, p, order);
    context.bps = bps;
    const ResidualSink sink{[](void* c, const int32_t* r, uint32_t m) {
                                Context& x = *static_cast<Context*>(c);
                                add_leaf_residuals(x.sums, r, m, x.bps);
                            },
                            &context};
    for_each_residual_chunk(signal, analysis.order, sink);
    const uint64_t bits = SUBFRAME_HEADER_BITS + (static_cast<uint64_t>(order) * bps) +
                          RESIDUAL_CODING_METHOD_BITS +
                          choose_partitions(context.sums, n, order, p, rice);
    if (rice.order != 0 && residual_bits_bound(bits, n, order) >= verbatim_subframe_bits(n, bps)) {
        return unpartitioned_fixed(analysis, rice);
    }
    return bits;
}

// Write the FIXED subframe partitioned as `rice`, which partition_fixed()
// kept only when its bound proves it smaller than VERBATIM
FLAC_NOINLINE void write_partitioned_fixed(BitWriterLocal& bw, const PackedSignal& signal,
                                           uint32_t bps, uint8_t order, const RicePartitions& rice,
                                           uint64_t estimated_bits) {
    const uint32_t n = signal.n;
    const uint64_t bound = residual_bits_bound(estimated_bits, n, order);
    struct Context {
        BitWriterLocal* bw;
        RiceEmitter emitter;
    } context{&bw, rice_emitter(rice, n, order, capacity_for(bw, bound + signal.wasted_bits()))};
    write_fixed_header(bw, signal, static_cast<uint8_t>(bps), order, rice.order, rice.k[0],
                       rice.parameter_bits);
    const ResidualSink sink{[](void* c, const int32_t* r, uint32_t m) {
                                Context& x = *static_cast<Context*>(c);
                                emit_partitioned(*x.bw, x.emitter, r, m);
                            },
                            &context};
    for_each_residual_chunk(signal, order, sink);
}

#endif  // MICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS

// How Pass B codes a frame's subframes. A build without partitioning reads
// none of the fields.
struct SubframeSettings {
    uint32_t max_partition_order;  // cppcheck-suppress unusedStructMember
};

// Write one subframe at depth bps: CONSTANT, FIXED (at its best
// partitioning), or VERBATIM when FIXED cannot be proven smaller.
//
// Out of line: encode_frame() calls it from three places.
FLAC_NOINLINE void write_subframe(BitWriterLocal& bw, const PackedSignal& signal, uint32_t bps,
                                  const ChannelAnalysis& analysis,
                                  const SubframeSettings& settings) {
    const uint8_t bps8 = static_cast<uint8_t>(bps);

    if (analysis.is_constant) {
        // Never with wasted bits, which would save only what they cost
        write_subframe_header(bw, SUBFRAME_TYPE_CONSTANT, 0);
        write_uint(bw, static_cast<uint32_t>(analysis.constant_value), bps8);
        return;
    }

#ifndef MICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS
    if (settings.max_partition_order != 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init) -- partition_fixed() sets it
        RicePartitions rice;
        const uint64_t fixed_bits =
            partition_fixed(signal, bps, analysis, settings.max_partition_order, rice);
        if (rice.order != 0) {
            write_partitioned_fixed(bw, signal, bps, analysis.order, rice, fixed_bits);
            return;
        }
    }
#else
    (void)settings;
#endif

    const uint64_t budget = fixed_subframe_budget(signal, bps, analysis);
    if (FLAC_UNLIKELY(budget == 0)) {
        write_verbatim_subframe(bw, signal, bps8);
        return;
    }
    // The budget bounds the whole subframe, so one check lets every code go
    // through the unchecked writer. The checked fallback is unreachable after
    // check_frame_room(), but keeps memory safety local to this function.
    const bool capacity_proven = capacity_for(bw, budget + signal.wasted_bits());

    const uint8_t order = analysis.order;
    const uint8_t k = analysis.k;
    write_fixed_header(bw, signal, bps8, order, 0, k, rice_parameter_bits(k));

    for_each_residual_chunk(signal, order, [&](const int32_t* r, uint32_t m) {
        emit_rice_codes(bw, r, m, k, capacity_proven);
    });
}

// ============================================================================
// Frame footer (RFC 9639 SS9.3)
// ============================================================================

// Pad to a byte boundary, append the CRC-16 of the whole frame, and flush.
// Returns the frame's size in bytes, or 0 if it overflowed the output.
FLAC_NOINLINE size_t close_frame(BitWriterLocal& bw, const uint8_t* frame_start) {
    write_align_zero(bw);
    const uint16_t crc = update_crc16(0, frame_start, static_cast<size_t>(bw.pos - frame_start));
    write_uint(bw, static_cast<uint32_t>((crc >> 8) & 0xFFU), 8);
    write_uint(bw, static_cast<uint32_t>(crc & 0xFFU), 8);
    return writer_flush(bw, frame_start);
}

}  // namespace

// ============================================================================
// FLACEncoder: Constants
// ============================================================================

#if __cplusplus < 201703L
// C++14 needs a definition for a static constexpr member a caller ODR-uses
// (binding it to a reference, as std::min() and std::max() do); C++17 makes
// them inline
constexpr size_t FLACEncoder::HEADER_BYTES;
constexpr uint32_t FLACEncoder::MIN_BLOCK_SIZE;
constexpr uint32_t FLACEncoder::MAX_BLOCK_SIZE;
#endif

// ============================================================================
// FLACEncoder: Lifecycle
// ============================================================================

FLACEncoder::FLACEncoder(const PcmFormat& format, const FLACEncoderOptions& options) {
    this->configure(format, options);
}

void FLACEncoder::reset() {
    this->total_samples_ = 0;
    this->frame_number_ = 0;
    this->min_frame_bytes_ = 0;
    this->max_frame_bytes_ = 0;
    this->finished_ = false;
}

void FLACEncoder::reset(const PcmFormat& format, const FLACEncoderOptions& options) {
    this->configure(format, options);
    this->reset();
}

// ============================================================================
// FLACEncoder: Core Encoding API
// ============================================================================

FLACEncoderResult FLACEncoder::write_header(uint8_t* output, size_t output_size_bytes,
                                            size_t& bytes_written) {
    bytes_written = 0;

    if (FLAC_UNLIKELY(this->config_result_ != FLAC_ENCODER_SUCCESS)) {
        return this->config_result_;
    }
    if (FLAC_UNLIKELY(output == nullptr)) {
        return FLAC_ENCODER_ERROR_INVALID_ARGUMENT;
    }
    if (FLAC_UNLIKELY(output_size_bytes < HEADER_BYTES)) {
        return FLAC_ENCODER_ERROR_OUTPUT_BUFFER_TOO_SMALL;
    }

    std::memcpy(output, MAGIC_BYTES, sizeof(MAGIC_BYTES));

    // STREAMINFO block header: last-block flag, type 0, then the length
    output[4] = 0x80;
    output[5] = 0x00;
    output[6] = 0x00;
    output[7] = static_cast<uint8_t>(STREAMINFO_SIZE);

    // STREAMINFO body (RFC 9639 SS8.2), big-endian
    // NOLINTBEGIN(readability-magic-numbers) -- byte offsets per FLAC STREAMINFO spec
    uint8_t* si = output + 8;
    std::memset(si, 0, STREAMINFO_SIZE);

    // Minimum and maximum block size: the last block is exempt from the minimum
    const uint32_t bs = this->options_.block_size;
    si[0] = static_cast<uint8_t>((bs >> 8) & 0xFFU);
    si[1] = static_cast<uint8_t>(bs & 0xFFU);
    si[2] = si[0];
    si[3] = si[1];

    // Frame sizes and total samples stay 0 ("unknown") until finish()
    uint32_t min_frame = 0;
    uint32_t max_frame = 0;
    uint64_t total_samples = 0;
    if (this->finished_) {
        if (this->max_frame_bytes_ <= FRAME_BYTES_MAX) {
            min_frame = this->min_frame_bytes_;
            max_frame = this->max_frame_bytes_;
        }
        if (this->total_samples_ <= TOTAL_SAMPLES_MAX) {
            total_samples = this->total_samples_;
        }
    }
    si[4] = static_cast<uint8_t>((min_frame >> 16) & 0xFFU);
    si[5] = static_cast<uint8_t>((min_frame >> 8) & 0xFFU);
    si[6] = static_cast<uint8_t>(min_frame & 0xFFU);
    si[7] = static_cast<uint8_t>((max_frame >> 16) & 0xFFU);
    si[8] = static_cast<uint8_t>((max_frame >> 8) & 0xFFU);
    si[9] = static_cast<uint8_t>(max_frame & 0xFFU);

    const uint32_t sr = this->format_.sample_rate();
    si[10] = static_cast<uint8_t>((sr >> 12) & 0xFFU);
    si[11] = static_cast<uint8_t>((sr >> 4) & 0xFFU);
    const uint8_t channels_minus_1 = static_cast<uint8_t>(this->format_.num_channels() - 1);
    const uint8_t bps_minus_1 = static_cast<uint8_t>(this->format_.bits_per_sample() - 1);
    // The 5-bit depth field straddles si[12] and si[13]
    si[12] =
        static_cast<uint8_t>(((sr & 0x0FU) << 4) | (static_cast<uint32_t>(channels_minus_1) << 1) |
                             (static_cast<uint32_t>(bps_minus_1) >> 4));
    si[13] = static_cast<uint8_t>(((bps_minus_1 & 0x0FU) << 4) |
                                  static_cast<uint32_t>((total_samples >> 32) & 0x0FU));
    si[14] = static_cast<uint8_t>((total_samples >> 24) & 0xFFU);
    si[15] = static_cast<uint8_t>((total_samples >> 16) & 0xFFU);
    si[16] = static_cast<uint8_t>((total_samples >> 8) & 0xFFU);
    si[17] = static_cast<uint8_t>(total_samples & 0xFFU);
    // si[18..33]: MD5 signature, 0 (unknown)
    // NOLINTEND(readability-magic-numbers)

    bytes_written = HEADER_BYTES;
    return FLAC_ENCODER_SUCCESS;
}

FLACEncoderResult FLACEncoder::encode(const uint8_t* input, size_t input_len, uint8_t* output,
                                      size_t output_size_bytes, size_t& bytes_consumed,
                                      size_t& bytes_written) {
    bytes_consumed = 0;
    bytes_written = 0;

    const FLACEncoderResult check = this->check_call(input, input_len, output);
    if (FLAC_UNLIKELY(check != FLAC_ENCODER_SUCCESS)) {
        return check;
    }
    if (input_len < this->input_block_bytes_) {
        return FLAC_ENCODER_NEED_MORE_DATA;
    }
    // A supported configuration's block is at least one byte, so check_call()
    // has rejected a null input by now
    FLAC_ASSUME(input != nullptr);
    const FLACEncoderResult room = this->check_frame_room(output_size_bytes);
    if (FLAC_UNLIKELY(room != FLAC_ENCODER_SUCCESS)) {
        return room;
    }

    const FLACEncoderResult result = this->encode_frame(input, this->options_.block_size, output,
                                                        output_size_bytes, bytes_written);
    if (result == FLAC_ENCODER_SUCCESS) {
        bytes_consumed = this->input_block_bytes_;
    }
    return result;
}

FLACEncoderResult FLACEncoder::finish(const uint8_t* input, size_t input_len, uint8_t* output,
                                      size_t output_size_bytes, size_t& bytes_written) {
    bytes_written = 0;

    const FLACEncoderResult check = this->check_call(input, input_len, output);
    if (FLAC_UNLIKELY(check != FLAC_ENCODER_SUCCESS)) {
        return check;
    }
    const size_t frame_bytes = this->format_.bytes_per_frame();
    if (input_len > this->input_block_bytes_ || (input_len % frame_bytes) != 0) {
        return FLAC_ENCODER_ERROR_INVALID_ARGUMENT;
    }

    if (input_len != 0) {
        const FLACEncoderResult room = this->check_frame_room(output_size_bytes);
        if (FLAC_UNLIKELY(room != FLAC_ENCODER_SUCCESS)) {
            return room;
        }
        // The one frame that may be shorter than the block size (RFC 9639 SS9.1.1)
        const uint32_t num_samples = static_cast<uint32_t>(input_len / frame_bytes);
        const FLACEncoderResult result =
            this->encode_frame(input, num_samples, output, output_size_bytes, bytes_written);
        if (result != FLAC_ENCODER_SUCCESS) {
            return result;
        }
    }

    this->finished_ = true;
    return FLAC_ENCODER_SUCCESS;
}

// ============================================================================
// FLACEncoder: Private Helpers
// ============================================================================

void FLACEncoder::configure(const PcmFormat& format, const FLACEncoderOptions& options) {
    this->format_ = format;
    this->options_ = options;

    // Clear everything derived from a previous configuration first, so a
    // rejected one leaves the same state as a freshly constructed encoder
    this->config_result_ = FLAC_ENCODER_ERROR_BAD_CONFIG;
    this->max_output_bytes_ = 0;
    this->input_block_bytes_ = 0;
    this->bits_per_sample_code_ = 0;
    this->sample_rate_code_ = 0;
    this->sample_rate_extra_[0] = 0;
    this->sample_rate_extra_[1] = 0;
    this->sample_rate_extra_len_ = 0;
    this->max_partition_order_ = 0;

    // Only validates and precomputes; config_result_ stays BAD_CONFIG unless
    // everything checks out.
    const uint32_t num_channels = format.num_channels();
    const uint32_t bits_per_sample = format.bits_per_sample();
    const uint32_t block_size = options.block_size;

    if (num_channels < 1 || num_channels > MAX_CHANNELS) {
        return;
    }
    if (bits_per_sample < MIN_BITS_PER_SAMPLE || bits_per_sample > MAX_BITS_PER_SAMPLE) {
        return;
    }
    if (block_size < MIN_BLOCK_SIZE || block_size > MAX_BLOCK_SIZE) {
        return;
    }
    if (options.max_rice_partition_order > MAX_RICE_PARTITION_ORDER) {
        return;
    }
    SampleRateCode rate;
    if (!select_sample_rate_code(format.sample_rate(), rate)) {
        return;
    }

    this->bits_per_sample_code_ = select_bits_per_sample_code(bits_per_sample);
    this->sample_rate_code_ = rate.code;
    this->sample_rate_extra_[0] = rate.extra[0];
    this->sample_rate_extra_[1] = rate.extra[1];
    this->sample_rate_extra_len_ = rate.extra_len;
#ifndef MICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS
    this->max_partition_order_ = options.max_rice_partition_order;
#endif

    // The worst frame is all VERBATIM (see fixed_subframe_budget()), with a
    // stereo side subframe one bit deeper. Per channel: a subframe header
    // byte, the samples and a byte of slack.
    const size_t bound_bps = bits_per_sample + ((num_channels == 2) ? 1U : 0U);
    const size_t verbatim_payload_bytes = (static_cast<size_t>(block_size) * bound_bps + 7) / 8;
    const size_t per_channel_bytes = 1 + verbatim_payload_bytes + 1;
    const size_t frame_bound = FRAME_HEADER_MAX_WRITE_LENGTH +
                               static_cast<size_t>(num_channels) * per_channel_bytes +
                               1     // byte-alignment padding
                               + 2;  // CRC-16
    this->max_output_bytes_ = (frame_bound > HEADER_BYTES) ? frame_bound : HEADER_BYTES;
    this->input_block_bytes_ = static_cast<size_t>(block_size) * format.bytes_per_frame();

    this->config_result_ = FLAC_ENCODER_SUCCESS;
}

FLACEncoderResult FLACEncoder::check_call(const uint8_t* input, size_t input_len,
                                          const uint8_t* output) {
    if (FLAC_UNLIKELY(this->config_result_ != FLAC_ENCODER_SUCCESS)) {
        return this->config_result_;
    }
    if (FLAC_UNLIKELY(this->finished_)) {
        return FLAC_ENCODER_ERROR_STREAM_FINISHED;
    }
    // NOLINTBEGIN(readability-simplify-boolean-expr) -- FLAC_UNLIKELY's !!(x)
    if (FLAC_UNLIKELY((input == nullptr && input_len != 0) || output == nullptr)) {
        return FLAC_ENCODER_ERROR_INVALID_ARGUMENT;
    }
    // 2-byte samples are read as int16_t, which faults unaligned on Xtensa
    if (FLAC_UNLIKELY(input_len != 0 && this->format_.bytes_per_sample() == 2 &&
                      (reinterpret_cast<uintptr_t>(input) & 1U) != 0)) {
        return FLAC_ENCODER_ERROR_INVALID_ARGUMENT;
    }
    // NOLINTEND(readability-simplify-boolean-expr)
    return FLAC_ENCODER_SUCCESS;
}

FLACEncoderResult FLACEncoder::check_frame_room(size_t output_size_bytes) const {
    // The coded frame number must fit in 31 bits (RFC 9639 SS9.1.5)
    if (FLAC_UNLIKELY(this->frame_number_ > FRAME_NUMBER_MAX)) {
        return FLAC_ENCODER_ERROR_STREAM_TOO_LONG;
    }
    if (FLAC_UNLIKELY(output_size_bytes < this->max_output_bytes_)) {
        return FLAC_ENCODER_ERROR_OUTPUT_BUFFER_TOO_SMALL;
    }
    return FLAC_ENCODER_SUCCESS;
}

FLAC_NOINLINE uint8_t FLACEncoder::start_frame(uint8_t* output, uint32_t num_samples,
                                               uint8_t channel_assignment) const {
    FrameHeaderFields fields;
    fields.frame_number = this->frame_number_;
    fields.block_size = num_samples;
    fields.channel_assignment = channel_assignment;
    fields.bits_per_sample_code = this->bits_per_sample_code_;
    fields.sample_rate.code = this->sample_rate_code_;
    fields.sample_rate.extra[0] = this->sample_rate_extra_[0];
    fields.sample_rate.extra[1] = this->sample_rate_extra_[1];
    fields.sample_rate.extra_len = this->sample_rate_extra_len_;
    return write_frame_header(output, fields);
}

FLAC_NOINLINE FLACEncoderResult FLACEncoder::finish_frame(size_t frame_bytes, uint32_t num_samples,
                                                          size_t& bytes_written) {
    if (FLAC_UNLIKELY(frame_bytes == 0)) {
        // Should be unreachable: the output capacity was already checked
        // against get_max_output_bytes() in check_frame_room(), and
        // write_subframe()'s proven-smaller FIXED check guarantees no frame
        // can exceed that bound. Kept as defense in depth.
        return FLAC_ENCODER_ERROR_OUTPUT_BUFFER_TOO_SMALL;
    }

    // max_output_bytes_ keeps frame_bytes well inside uint32_t
    const uint32_t frame_bytes32 = static_cast<uint32_t>(frame_bytes);
    if (this->min_frame_bytes_ == 0 || frame_bytes32 < this->min_frame_bytes_) {
        this->min_frame_bytes_ = frame_bytes32;
    }
    if (frame_bytes32 > this->max_frame_bytes_) {
        this->max_frame_bytes_ = frame_bytes32;
    }

    this->frame_number_++;
    this->total_samples_ += num_samples;
    bytes_written = frame_bytes;
    return FLAC_ENCODER_SUCCESS;
}

// ============================================================================
// FLACEncoder: Frame Encoding
// ============================================================================

FLACEncoderResult FLACEncoder::encode_frame(const uint8_t* input, uint32_t num_samples,
                                            uint8_t* output, size_t output_size_bytes,
                                            size_t& bytes_written) {
    const uint32_t bps = this->format_.bits_per_sample();
    const uint32_t side_bps = bps + 1;
    const uint32_t nch = this->format_.num_channels();
    const uint32_t bytes = this->format_.bytes_per_sample();

    const uint32_t chunk_len = CHUNK_SAMPLES;
    int32_t* const a = this->chunk_;
    int32_t* const b = this->chunk_ + chunk_len;
    const uint32_t warmup = scan_warmup_samples(num_samples);

    // Each signal of this frame is `base` with its channel or candidate set
    const PackedSignal base{input, num_samples,    bps, bytes,     nch,
                            0,     SignalId::LEFT, a,   chunk_len, 0};
    const SubframeSettings settings{this->max_partition_order_};
    const bool find_wasted = this->options_.wasted_bits;

    if (nch != 2) {
        // No assignment to choose: analyze and write each channel in turn
        BitWriterLocal bw{};
        const uint8_t header_len =
            this->start_frame(output, num_samples, static_cast<uint8_t>(nch - 1));
        writer_init(bw, output + header_len, output_size_bytes - header_len);

        for (uint32_t ch = 0; ch < nch; ch++) {
            PackedSignal signal = base;
            signal.channel = ch;

            SignalScan scan{};
            uint32_t bits = 0;
            for (uint32_t start = 0; start < num_samples; start += chunk_len) {
                const uint32_t m =
                    (num_samples - start > chunk_len) ? chunk_len : (num_samples - start);
                const int32_t* s = signal.unpack(start, m);
                uint32_t begin = 0;
                if (start == 0) {
                    scan_begin(scan, s, warmup);
                    begin = warmup;
                }
                scan_chunk(scan, s, begin, m, bps);
                if (find_wasted) {
                    bits |= or_samples(s, m);
                }
            }
            const ChannelAnalysis analysis =
                finish_wasted_analysis(scan, num_samples, bps, bits, signal.wasted);
            write_subframe(bw, signal, bps - signal.wasted, analysis, settings);
        }

        return this->finish_frame(close_frame(bw, output), num_samples, bytes_written);
    }

    // ---- Stereo Pass A: one walk analyzes all four candidates ----
    //
    // Each chunk is unpacked to [left, right] and scanned, then converted to
    // [mid, side] in place and scanned again.
    const bool estimate_stereo = this->stereo_estimation_enabled_;
    SignalScan scan_l{};
    SignalScan scan_r{};
    SignalScan scan_mid{};
    SignalScan scan_side{};
    uint32_t bits[4] = {0, 0, 0, 0};  // Each candidate's samples ORed, indexed by SignalId
    for (uint32_t start = 0; start < num_samples; start += chunk_len) {
        const uint32_t m = (num_samples - start > chunk_len) ? chunk_len : (num_samples - start);
        unpack_stereo(a, b, input + (static_cast<size_t>(start) * 2 * bytes), m, bps, bytes);
        const bool first = (start == 0);
        const uint32_t begin = first ? warmup : 0;
        if (first) {
            scan_begin(scan_l, a, warmup);
            scan_begin(scan_r, b, warmup);
        }
        scan_chunk(scan_l, a, begin, m, bps);
        scan_chunk(scan_r, b, begin, m, bps);
        if (estimate_stereo) {
            if (find_wasted) {
                left_right_to_mid_side_or(a, b, m, bits);
            } else {
                left_right_to_mid_side(a, b, m);
            }
            if (first) {
                scan_begin(scan_mid, a, warmup);
                scan_begin(scan_side, b, warmup);
            }
            scan_chunk(scan_mid, a, begin, m, bps);
            scan_chunk(scan_side, b, begin, m, side_bps);
        } else if (find_wasted) {
            bits[0] |= or_samples(a, m);
            bits[1] |= or_samples(b, m);
        }
    }

    // Wasted bits are found per candidate, mid from its own samples: when left
    // and right share k, mid may keep only k - 1, since the decoder rebuilds
    // mid's dropped low bit from side's.
    uint32_t wasted[4] = {0, 0, 0, 0};  // Indexed by SignalId
    const ChannelAnalysis analysis_l =
        finish_wasted_analysis(scan_l, num_samples, bps, bits[0], wasted[0]);
    const ChannelAnalysis analysis_r =
        finish_wasted_analysis(scan_r, num_samples, bps, bits[1], wasted[1]);
    StereoPlan plan{};
    plan.channel_assignment = static_cast<uint8_t>(CHANNEL_INDEPENDENT_STEREO);
    plan.sig0 = SignalId::LEFT;
    plan.bps0 = bps;
    plan.analysis0 = analysis_l;
    plan.sig1 = SignalId::RIGHT;
    plan.bps1 = bps;
    plan.analysis1 = analysis_r;
    if (estimate_stereo) {
        const ChannelAnalysis analysis_mid =
            finish_wasted_analysis(scan_mid, num_samples, bps, bits[2], wasted[2]);
        const ChannelAnalysis analysis_side =
            finish_wasted_analysis(scan_side, num_samples, side_bps, bits[3], wasted[3]);
        plan = choose_stereo_plan(analysis_l, analysis_r, analysis_mid, analysis_side, bps);
    }

    BitWriterLocal bw{};
    const uint8_t header_len = this->start_frame(output, num_samples, plan.channel_assignment);
    writer_init(bw, output + header_len, output_size_bytes - header_len);

    // ---- Stereo Pass B ----
    PackedSignal signal0 = base;
    signal0.sig = plan.sig0;
    signal0.wasted = wasted[static_cast<uint32_t>(plan.sig0)];
    write_subframe(bw, signal0, plan.bps0 - signal0.wasted, plan.analysis0, settings);
    PackedSignal signal1 = base;
    signal1.sig = plan.sig1;
    signal1.wasted = wasted[static_cast<uint32_t>(plan.sig1)];
    write_subframe(bw, signal1, plan.bps1 - signal1.wasted, plan.analysis1, settings);

    return this->finish_frame(close_frame(bw, output), num_samples, bytes_written);
}

}  // namespace micro_flac
