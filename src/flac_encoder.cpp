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
#include "lpc_analysis.h"
#include "pcm_packing.h"
#include "rice_partitions.h"
#include "subframe_coding.h"

#include <cstdint>
#include <cstring>

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

#ifndef MICRO_FLAC_ENCODER_DISABLE_LPC
// LPC applies to every depth Pass A scans in 32 bits: the deepest such
// stream's side subframe must fit LPC_MAX_SUBFRAME_BPS (lpc_analysis.h).
static_assert(LPC_MAX_SUBFRAME_BPS == MAX_SCAN32_BITS_PER_SAMPLE + 1,
              "LPC must cover the deepest 32-bit-scanned stream's side subframe");
static_assert(FLACEncoder::MAX_LPC_BITS_PER_SAMPLE == MAX_SCAN32_BITS_PER_SAMPLE,
              "the public LPC depth limit must match the encoder's");
#endif
static_assert(FLACEncoder::MAX_LPC_ORDER == LPC_MAX_ORDER,
              "the public LPC order limit must match the analysis's");

#ifndef MICRO_FLAC_ENCODER_DISABLE_LPC  // LPC only

// LPC coefficient precision to aim for (libFLAC's defaults): by block size at
// 16 bits, 2 + bps/2 (at least 5) below that, and 13-15 by block size above
// it. lpc_design() may lower it to keep the narrow path's residuals in 32 bits.
uint32_t lpc_precision(uint32_t bits_per_sample, uint32_t block_size) {
    // NOLINTBEGIN(readability-magic-numbers) -- libFLAC's precision table
    if (bits_per_sample < 16) {
        const uint32_t p = 2 + (bits_per_sample / 2);
        return (p < 5) ? 5 : p;
    }
    if (bits_per_sample > 16) {
        return (block_size <= 384) ? 13 : ((block_size <= 1152) ? 14 : 15);
    }
    if (block_size <= 192) {
        return 7;
    }
    if (block_size <= 384) {
        return 8;
    }
    if (block_size <= 576) {
        return 9;
    }
    if (block_size <= 1152) {
        return 10;
    }
    if (block_size <= 2304) {
        return 11;
    }
    if (block_size <= 4608) {
        return 12;
    }
    return 13;
    // NOLINTEND(readability-magic-numbers)
}

#endif  // MICRO_FLAC_ENCODER_DISABLE_LPC

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
}

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

// Every fixed order, as finish_analysis()'s `orders` mask
constexpr uint32_t ALL_FIXED_ORDERS = (1U << (MAX_FIXED_ORDER + 1)) - 1;

// Pick the fixed-predictor order and Rice parameter with the lowest
// estimated size from a finished scan's sums, at subframe depth bps (one
// more than the stream's for a side signal), among the orders set in
// `orders`; with none, VERBATIM. Out of line: it runs once per signal, off
// the hot path.
FLAC_NOINLINE ChannelAnalysis finish_analysis(const uint64_t (&sums)[MAX_FIXED_ORDER + 1],
                                              uint32_t n, uint32_t bps, uint32_t orders) {
    ChannelAnalysis result{};

    // Every first difference was zero <=> every sample equals the first.
    const bool constant = (sums[1] == 0);

    result.is_constant = constant;
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
        if (((orders >> order) & 1U) == 0) {
            continue;
        }
        const uint32_t residual_count = n - order;
        const uint64_t sum = sums[order];
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
    const uint64_t verbatim_bits = verbatim_subframe_bits(n, bps);
    if (FLAC_UNLIKELY(best_bits == UINT64_MAX)) {
        result.verbatim = true;
        result.estimated_bits = verbatim_bits;
        return result;
    }
    best_bits += rice_parameter_bits(best_k) - RICE_PARAMETER_BITS;
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
    return finish_analysis(scan.sum, n, bps - wasted, ALL_FIXED_ORDERS);
}

// ============================================================================
// Pass A in 64 bits, for streams deeper than MAX_SCAN32_BITS_PER_SAMPLE
// ============================================================================
//
// Their residuals outgrow scan_range()'s 32 bits (32-bit stereo's side
// reaches 2^36 at order 4), and a Rice-coded residual must stay below 2^31
// (RFC 9639 SS9.2.7.3). scan64() also ORs each order's magnitudes: the OR is
// below 2^31 exactly when every magnitude is, and shifts out wasted bits with
// them. Pass B stays in 32 bits (see cascade_step()).

// scan_range()'s state, in 64 bits, with each order's magnitudes ORed
struct Scan64 {
    uint64_t sum[MAX_FIXED_ORDER + 1];
    uint64_t magnitude_or[MAX_FIXED_ORDER + 1];
    int64_t prev[MAX_FIXED_ORDER];
    uint32_t bits;   // The samples' low 32 bits ORed, for their wasted bits
    uint32_t count;  // Samples scanned so far
};

// Continue a 64-bit scan over m samples, sample(i) giving each
template <typename Sample>
FLAC_COLD_SECTION FLAC_NOINLINE void scan64(Scan64& scan, uint32_t m, Sample sample) {
    Scan64 s = scan;  // A copy the loops can keep in registers
    // Add sample i's residuals of orders up to `orders`
    const auto step = [&](uint32_t i, uint32_t orders) {
        const int64_t s0 = sample(i);
        const int64_t e[MAX_FIXED_ORDER + 1] = {s0, s0 - s.prev[0], s0 - s.prev[0] - s.prev[1],
                                                s0 - s.prev[0] - s.prev[1] - s.prev[2],
                                                s0 - s.prev[0] - s.prev[1] - s.prev[2] - s.prev[3]};
        for (uint32_t o = 0; o <= orders; o++) {
            const uint64_t magnitude =
                (e[o] < 0) ? (0U - static_cast<uint64_t>(e[o])) : static_cast<uint64_t>(e[o]);
            s.sum[o] += magnitude;
            s.magnitude_or[o] |= magnitude;
        }
        s.bits |= static_cast<uint32_t>(static_cast<uint64_t>(s0));
        s.prev[3] = e[3];
        s.prev[2] = e[2];
        s.prev[1] = e[1];
        s.prev[0] = s0;
    };
    // Order o's first residual is at sample o
    uint32_t i = 0;
    for (; i < m && s.count + i < MAX_FIXED_ORDER; i++) {
        step(i, s.count + i);
    }
    for (; i < m; i++) {
        step(i, MAX_FIXED_ORDER);
    }
    s.count += m;
    scan = s;
}

// finish_wasted_analysis() for a 64-bit scan, among the orders whose
// residuals stay below 2^31
FLAC_COLD_SECTION FLAC_NOINLINE ChannelAnalysis finish_analysis64(const Scan64& scan, uint32_t n,
                                                                  uint32_t bps, bool find_wasted,
                                                                  uint32_t& wasted) {
    uint64_t sums[MAX_FIXED_ORDER + 1];
    wasted = 0;
    if (find_wasted && scan.bits != 0 && scan.sum[1] != 0) {
        wasted = static_cast<uint32_t>(__builtin_ctz(scan.bits));
    }
    uint32_t orders = 0;
    for (uint32_t o = 0; o <= MAX_FIXED_ORDER; o++) {
        sums[o] = scan.sum[o] >> wasted;
        if ((scan.magnitude_or[o] >> wasted) < RESIDUAL_MAGNITUDE_LIMIT) {
            orders |= 1U << o;
        }
    }
    return finish_analysis(sums, n, bps - wasted, orders);
}

// 64-bit Pass A for one channel
FLAC_COLD_SECTION FLAC_NOINLINE ChannelAnalysis analyze_channel64(const PackedSignal& signal,
                                                                  bool find_wasted,
                                                                  uint32_t& wasted) {
    Scan64 scan{};
    for (uint32_t start = 0; start < signal.n; start += signal.chunk_len) {
        const uint32_t m =
            (signal.n - start > signal.chunk_len) ? signal.chunk_len : (signal.n - start);
        const int32_t* s = signal.unpack(start, m);
        scan64(scan, m, [s](uint32_t i) { return static_cast<int64_t>(s[i]); });
    }
    return finish_analysis64(scan, signal.n, signal.bps, find_wasted, wasted);
}

// 64-bit Pass A for stereo: left and right, plus mid and side with
// `estimate_stereo`. `analyses` and `wasted` are indexed by SignalId.
FLAC_COLD_SECTION FLAC_NOINLINE void analyze_stereo64(const PackedSignal& base,
                                                      bool estimate_stereo, bool find_wasted,
                                                      ChannelAnalysis (&analyses)[4],
                                                      uint32_t (&wasted)[4]) {
    Scan64 scans[4] = {};
    int32_t* const left = base.buf;
    int32_t* const right = base.buf + base.chunk_len;
    const size_t frame_bytes = static_cast<size_t>(base.bytes) * 2;
    for (uint32_t start = 0; start < base.n; start += base.chunk_len) {
        const uint32_t m = (base.n - start > base.chunk_len) ? base.chunk_len : (base.n - start);
        unpack_stereo32(left, right, base.input + (start * frame_bytes), m, base.bps);
        scan64(scans[0], m, [left](uint32_t i) { return static_cast<int64_t>(left[i]); });
        scan64(scans[1], m, [right](uint32_t i) { return static_cast<int64_t>(right[i]); });
        if (estimate_stereo) {
            scan64(scans[2], m, [left, right](uint32_t i) {
                return (static_cast<int64_t>(left[i]) + right[i]) >> 1;
            });
            scan64(scans[3], m,
                   [left, right](uint32_t i) { return static_cast<int64_t>(left[i]) - right[i]; });
        }
    }
    const uint32_t count = estimate_stereo ? 4 : 2;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t bps = (i == static_cast<uint32_t>(SignalId::SIDE)) ? base.bps + 1 : base.bps;
        analyses[i] = finish_analysis64(scans[i], base.n, bps, find_wasted, wasted[i]);
    }
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

// Pass B's size bound (residual_bits_bound()) for Pass A's unpartitioned
// FIXED subframe
FLAC_ALWAYS_INLINE uint64_t fixed_subframe_bits_bound(const ChannelAnalysis& analysis, uint32_t n) {
    return residual_bits_bound(analysis.estimated_bits, n, analysis.order);
}

// Exact size of the FIXED subframe, by another walk over its residuals. The
// rare path behind fixed_subframe_bits_bound(), so it stays out of line.
// A ChannelAnalysis::verbatim signal, estimated at VERBATIM's size, also
// lands here and is priced out.
FLAC_NOINLINE uint64_t fixed_subframe_exact_bits(const PackedSignal& signal, uint32_t bps,
                                                 const ChannelAnalysis& analysis) {
    if (FLAC_UNLIKELY(analysis.verbatim)) {
        return UINT64_MAX;
    }
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

// VERBATIM subframe: the header, then every sample
FLAC_NOINLINE void write_verbatim_subframe(BitWriterLocal& bw, const PackedSignal& signal,
                                           uint8_t bps8) {
    write_subframe_header(bw, SUBFRAME_TYPE_VERBATIM, signal.wasted_bits());
    if (FLAC_UNLIKELY(bps8 > 32)) {
        // A 33-bit side, which unpack() wraps: derive it from left and right
        int32_t* const left = signal.buf;
        int32_t* const right = signal.buf + signal.chunk_len;
        const size_t frame_bytes = static_cast<size_t>(signal.bytes) * 2;
        for (uint32_t start = 0; start < signal.n; start += signal.chunk_len) {
            const uint32_t m =
                (signal.n - start > signal.chunk_len) ? signal.chunk_len : (signal.n - start);
            unpack_stereo32(left, right, signal.input + (start * frame_bytes), m, signal.bps);
            for (uint32_t i = 0; i < m; i++) {
                write_sample(bw, static_cast<int64_t>(left[i]) - right[i], bps8);
            }
        }
        return;
    }
    auto sink = [&](const int32_t* s, uint32_t m) {
        for (uint32_t i = 0; i < m; i++) {
            write_uint(bw, static_cast<uint32_t>(s[i]), bps8);
        }
    };
    signal.residual_chunks<0>(sink);
}

// CONSTANT subframe: the header, then the first sample. Never with wasted
// bits, which would save only what they cost. Out of line, keeping its
// 64-bit read out of write_subframe().
FLAC_NOINLINE void write_constant_subframe(BitWriterLocal& bw, const PackedSignal& signal,
                                           uint8_t bps8) {
    write_subframe_header(bw, SUBFRAME_TYPE_CONSTANT, 0);
    write_sample(bw, signal.warmup_sample(0), bps8);
}

// ----------------------------------------------------------------------------
// LPC subframes (FLACEncoderOptions::lpc)
// ----------------------------------------------------------------------------
//
// Pass A picks the stereo assignment from the fixed predictors, and Pass B
// tries LPC on the two subframes the frame carries (the stereo search below
// tries all four). An attempt walks the signal three times: the windowed
// autocorrelation, a residual magnitude sum that prices the designed
// predictor in Pass A's terms (so fixed_subframe_bits_bound()'s proof covers
// it), and, if LPC wins, emission.

// How Pass B codes a frame's subframes. lpc_max_order 0 means no LPC. A
// build without LPC or partitioning reads none of the fields it leaves out.
struct SubframeSettings {
    uint32_t lpc_max_order;        // cppcheck-suppress unusedStructMember
    uint32_t lpc_precision;        // cppcheck-suppress unusedStructMember
    uint32_t max_partition_order;  // cppcheck-suppress unusedStructMember
};

// Pass B's signatures carry a design pointer in every build; only LPC defines it.
struct LpcCandidate;

#ifndef MICRO_FLAC_ENCODER_DISABLE_LPC  // LPC only

// A designed predictor, its Rice coding and its estimated subframe size
struct LpcCandidate {
    LpcPredictor predictor;
    RicePartitions rice;
    uint64_t estimated_bits;
};

// The kernels, pricing and header writer are out of line and shared, so the
// walks themselves stay small.

// Add a chunk's residual magnitudes to abs_sum, a kernel-sized piece at a time
FLAC_NOINLINE void lpc_sum_chunk(const LpcPredictor& predictor, LpcResidualState& state,
                                 const int32_t* samples, uint32_t m, int32_t* work,
                                 uint64_t& abs_sum) {
    for (uint32_t off = 0; off < m; off += LPC_KERNEL_SAMPLES) {
        const uint32_t len = (m - off > LPC_KERNEL_SAMPLES) ? LPC_KERNEL_SAMPLES : (m - off);
        abs_sum += lpc_residual_sum(predictor, state, samples + off, len, work);
    }
}

// Write a chunk's residuals' Rice codes, a kernel-sized piece at a time
FLAC_NOINLINE void lpc_emit_chunk(BitWriterLocal& bw, const LpcPredictor& predictor,
                                  LpcResidualState& state, const int32_t* samples, uint32_t m,
                                  int32_t* work, uint8_t k, bool capacity_proven) {
    for (uint32_t off = 0; off < m; off += LPC_KERNEL_SAMPLES) {
        const uint32_t len = (m - off > LPC_KERNEL_SAMPLES) ? LPC_KERNEL_SAMPLES : (m - off);
        uint32_t count = 0;
        const int32_t* r = lpc_residuals(predictor, state, samples + off, len, work, count);
        emit_rice_codes(bw, r, count, k, capacity_proven);
    }
}

#ifndef MICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS

// lpc_sum_chunk() into leaf sums, each piece within one leaf
FLAC_NOINLINE void lpc_sum_leaves(const LpcPredictor& predictor, LpcResidualState& state,
                                  const int32_t* samples, uint32_t m, int32_t* work,
                                  LeafSums& sums) {
    for (uint32_t off = 0; off < m;) {
        const uint32_t piece = (m - off > LPC_KERNEL_SAMPLES) ? LPC_KERNEL_SAMPLES : (m - off);
        const uint32_t len = leaf_step(sums, piece);
        // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign) -- as in choose_partitions()
        sums.sum[sums.index] += lpc_residual_sum(predictor, state, samples + off, len, work);
        sums.pos += len;
        off += len;
    }
}

// lpc_emit_chunk() for a split subframe
FLAC_NOINLINE void lpc_emit_partitioned(BitWriterLocal& bw, const LpcPredictor& predictor,
                                        LpcResidualState& state, const int32_t* samples, uint32_t m,
                                        int32_t* work, RiceEmitter& emitter) {
    for (uint32_t off = 0; off < m; off += LPC_KERNEL_SAMPLES) {
        const uint32_t len = (m - off > LPC_KERNEL_SAMPLES) ? LPC_KERNEL_SAMPLES : (m - off);
        uint32_t count = 0;
        const int32_t* r = lpc_residuals(predictor, state, samples + off, len, work, count);
        emit_partitioned(bw, emitter, r, count);
    }
}

#endif  // MICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS

// Bits of an LPC subframe's header up to its partition order: subframe
// header, warmup, precision, shift, coefficients and coding method
FLAC_ALWAYS_INLINE uint64_t lpc_header_bits(const LpcPredictor& predictor, uint32_t bps) {
    return SUBFRAME_HEADER_BITS + (static_cast<uint64_t>(predictor.order) * bps) +
           LPC_PRECISION_BITS + LPC_SHIFT_BITS +
           (static_cast<uint64_t>(predictor.order) * predictor.precision) +
           RESIDUAL_CODING_METHOD_BITS;
}

// Price a designed predictor from its residual magnitude sum, in the same
// terms as Pass A's FIXED estimate: the header exactly, the codes by
// estimate_rice_bits().
//
// Both sides deliberately share that estimate's bias. At k = 0 it assumes
// half the residuals are negative, so a near-perfect predictor can win a
// comparison it would lose exactly, by up to a few percent on synthetic
// signals and a byte or two on real audio. Pricing only LPC exactly
// made the output larger on average.
FLAC_NOINLINE void lpc_price(LpcCandidate& candidate, uint64_t abs_sum, uint32_t n, uint32_t bps) {
    const uint32_t count = n - candidate.predictor.order;
    uint8_t k = 0;
    const uint64_t codes = estimate_rice(abs_sum, count, k);
    candidate.rice.order = 0;
    candidate.rice.parameter_bits = static_cast<uint8_t>(rice_parameter_bits(k));
    candidate.rice.k[0] = k;
    candidate.estimated_bits = lpc_header_bits(candidate.predictor, bps) +
                               RICE_PARTITION_ORDER_BITS + candidate.rice.parameter_bits + codes;
}

// Upper bound on the LPC subframe's size, as in fixed_subframe_bits_bound()
FLAC_ALWAYS_INLINE uint64_t lpc_bound(const LpcCandidate& candidate, uint32_t n) {
    return residual_bits_bound(candidate.estimated_bits, n, candidate.predictor.order);
}

// LPC subframe header (RFC 9639 SS9.2.6) through the first Rice parameter
FLAC_NOINLINE void write_lpc_header(BitWriterLocal& bw, const LpcCandidate& candidate,
                                    const int32_t* warmup, uint8_t bps8, uint32_t wasted) {
    const LpcPredictor& predictor = candidate.predictor;
    write_subframe_header(bw, static_cast<uint8_t>(SUBFRAME_TYPE_LPC_MIN + predictor.order - 1),
                          wasted);
    for (uint32_t i = 0; i < predictor.order; i++) {
        write_uint(bw, static_cast<uint32_t>(warmup[i]), bps8);
    }
    write_uint(bw, predictor.precision - 1, LPC_PRECISION_BITS);
    write_uint(bw, predictor.shift, LPC_SHIFT_BITS);
    const uint8_t precision8 = static_cast<uint8_t>(predictor.precision);
    for (uint32_t i = 0; i < predictor.order; i++) {
        write_uint(bw, static_cast<uint32_t>(predictor.coefficients[i]), precision8);
    }
    write_coding_method(bw, candidate.rice.parameter_bits);
    write_uint(bw, FLACEncoder::RICE_PARTITIONS_AVAILABLE ? candidate.rice.order : 0,
               RICE_PARTITION_ORDER_BITS);
    write_uint(bw, candidate.rice.k[0], candidate.rice.parameter_bits);
}

// Whether the signal's stream is deeper than 16 bits, which takes the wide
// LPC path
FLAC_ALWAYS_INLINE bool deep_stream(const PackedSignal& signal) {
    return signal.bps > 16;
}

// Walk 2: price the designed predictor from its residual magnitudes, per
// leaf partition when the subframe can split. Returns false as design_lpc()
// does. Inlined so design_lpc()'s walks share one stack frame.
FLAC_ALWAYS_INLINE bool price_lpc(const PackedSignal& signal, uint32_t bps,
                                  const SubframeSettings& settings, LpcCandidate& candidate,
                                  int32_t* work) {
    const uint32_t n = signal.n;
    const LpcPredictor& predictor = candidate.predictor;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init) -- lpc_residual_begin() sets it
    LpcResidualState state;
    lpc_residual_begin(state);
#ifndef MICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS
    const uint32_t p = partition_order_limit(n, predictor.order, settings.max_partition_order);
    if (p != 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init) -- leaf_sums_begin() sets it
        LeafSums sums;
        leaf_sums_begin(sums, n, p, 0);
        auto sum = [&](const int32_t* s, uint32_t m) {
            lpc_sum_leaves(predictor, state, s, m, work, sums);
        };
        signal.residual_chunks<0>(sum);
        if (!state.in_range) {
            return false;
        }
        candidate.estimated_bits = lpc_header_bits(predictor, bps) +
                                   choose_partitions(sums, n, predictor.order, p, candidate.rice);
        return lpc_bound(candidate, n) < verbatim_subframe_bits(n, bps);
    }
#else
    (void)settings;
#endif
    uint64_t abs_sum = 0;
    auto sum = [&](const int32_t* s, uint32_t m) {
        lpc_sum_chunk(predictor, state, s, m, work, abs_sum);
    };
    signal.residual_chunks<0>(sum);
    if (!state.in_range) {
        return false;
    }
    lpc_price(candidate, abs_sum, n, bps);
    return lpc_bound(candidate, n) < verbatim_subframe_bits(n, bps);
}

// Walks 1 and 2: design and price the signal's LPC candidate. Returns false
// when LPC cannot code it (a degenerate autocorrelation or a residual out of
// range) or cannot be proven smaller than VERBATIM.
FLAC_NOINLINE bool design_lpc(const PackedSignal& signal, uint32_t bps,
                              const SubframeSettings& settings, LpcCandidate& candidate) {
    const uint32_t n = signal.n;
    if (n < 2) {
        return false;  // No residual after the warmup
    }
    int32_t work[LPC_WORK_SAMPLES];

    // Walk 1: the windowed autocorrelation and the design, scoped so walk 2
    // can reuse its stack.
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init) -- set by its begin()
        LpcAutocorrelation ac;
        const uint32_t max_order = settings.lpc_max_order;
        const uint32_t lags = (max_order < n) ? max_order : (n - 1);
        if (deep_stream(signal)) {
            lpc_autocorrelation_begin_wide(ac, n, lags, bps);
        } else {
            lpc_autocorrelation_begin(ac, n, lags, bps);
        }
        auto correlate = [&](const int32_t* s, uint32_t m) {
            lpc_autocorrelation_update(ac, s, m, work);
        };
        signal.residual_chunks<0>(correlate);
        if (!lpc_design(ac, bps, settings.lpc_precision, candidate.predictor)) {
            return false;
        }
    }
    return price_lpc(signal, bps, settings, candidate, work);
}

// Walk 3: write the LPC subframe design_lpc() designed
FLAC_NOINLINE void write_lpc_subframe(BitWriterLocal& bw, const PackedSignal& signal, uint32_t bps,
                                      const LpcCandidate& candidate) {
    int32_t work[LPC_WORK_SAMPLES];
    const LpcPredictor& predictor = candidate.predictor;
    const bool capacity_proven =
        capacity_for(bw, lpc_bound(candidate, signal.n) + signal.wasted_bits());
    int32_t warmup[LPC_MAX_ORDER] = {};
    for (uint32_t i = 0; i < predictor.order; i++) {
        // Fits: LPC streams are at most MAX_LPC_BITS_PER_SAMPLE deep
        warmup[i] = static_cast<int32_t>(signal.warmup_sample(i));
    }
    write_lpc_header(bw, candidate, warmup, static_cast<uint8_t>(bps), signal.wasted_bits());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init) -- lpc_residual_begin() sets it
    LpcResidualState state;
    lpc_residual_begin(state);
#ifndef MICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS
    if (candidate.rice.order != 0) {
        RiceEmitter emitter =
            rice_emitter(candidate.rice, signal.n, predictor.order, capacity_proven);
        auto emit = [&](const int32_t* s, uint32_t m) {
            lpc_emit_partitioned(bw, predictor, state, s, m, work, emitter);
        };
        signal.residual_chunks<0>(emit);
        return;
    }
#endif
    const uint8_t k = candidate.rice.k[0];
    auto emit = [&](const int32_t* s, uint32_t m) {
        lpc_emit_chunk(bw, predictor, state, s, m, work, k, capacity_proven);
    };
    signal.residual_chunks<0>(emit);
}

#endif  // MICRO_FLAC_ENCODER_DISABLE_LPC

// Write one subframe at depth bps: CONSTANT, the cheaper of FIXED (at its
// best partitioning) and LPC, or VERBATIM when FIXED cannot be proven
// smaller. `designed` is the stereo search's LPC design for this signal, if
// any; otherwise LPC is designed here when enabled.
//
// Out of line: encode_frame() calls it from three places.
FLAC_NOINLINE void write_subframe(BitWriterLocal& bw, const PackedSignal& signal, uint32_t bps,
                                  const ChannelAnalysis& analysis, const SubframeSettings& settings,
                                  const LpcCandidate* designed) {
    const uint8_t bps8 = static_cast<uint8_t>(bps);

    if (analysis.is_constant) {
        write_constant_subframe(bw, signal, bps8);
        return;
    }

    uint64_t fixed_bits = analysis.estimated_bits;
#ifndef MICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init) -- order 0 reads nothing else
    RicePartitions rice;
    rice.order = 0;
    if (settings.max_partition_order != 0) {
        fixed_bits = partition_fixed(signal, bps, analysis, settings.max_partition_order, rice);
    }
#endif

#ifndef MICRO_FLAC_ENCODER_DISABLE_LPC
    if (designed != nullptr) {
        // The search hands over only designs that beat unpartitioned FIXED,
        // so without partitioning this folds away.
        if (!FLACEncoder::RICE_PARTITIONS_AVAILABLE || designed->estimated_bits < fixed_bits) {
            write_lpc_subframe(bw, signal, bps, *designed);
            return;
        }
    } else if (settings.lpc_max_order != 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init) -- design_lpc() sets it
        LpcCandidate candidate;
        if (design_lpc(signal, bps, settings, candidate) && candidate.estimated_bits < fixed_bits) {
            write_lpc_subframe(bw, signal, bps, candidate);
            return;
        }
    }
#else
    (void)settings;
    (void)designed;
    (void)fixed_bits;
#endif

#ifndef MICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS
    if (rice.order != 0) {
        write_partitioned_fixed(bw, signal, bps, analysis.order, rice, fixed_bits);
        return;
    }
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

// ----------------------------------------------------------------------------
// Stereo search (FLACEncoderOptions::lpc_stereo_search)
// ----------------------------------------------------------------------------
//
// The assignment cheapest with fixed predictors is often not the one
// cheapest with LPC. The search designs LPC for all four candidates, chooses
// from each one's better estimate, and hands the chosen two designs to Pass
// B, so only the unchosen two are extra work.

#ifndef MICRO_FLAC_ENCODER_DISABLE_LPC

// The chosen subframes' LPC designs, in wire order, where LPC beats FIXED
struct StereoLpc {
    LpcCandidate candidate[2];
    bool use[2];
};

// Choose the stereo assignment from each candidate's better estimate, FIXED
// or LPC (arrays indexed by SignalId). The plan keeps the FIXED analyses,
// which Pass B's size bound needs.
StereoPlan choose_stereo_plan_with_lpc(const ChannelAnalysis (&analyses)[4],
                                       const LpcCandidate (&designs)[4], const bool (&designed)[4],
                                       uint32_t bps, StereoLpc& out) {
    ChannelAnalysis priced[4];
    for (uint32_t i = 0; i < 4; i++) {
        priced[i] = analyses[i];
        if (designed[i] && designs[i].estimated_bits < priced[i].estimated_bits) {
            priced[i].estimated_bits = designs[i].estimated_bits;
        }
    }
    StereoPlan plan = choose_stereo_plan(priced[0], priced[1], priced[2], priced[3], bps);
    const uint32_t chosen[2] = {static_cast<uint32_t>(plan.sig0), static_cast<uint32_t>(plan.sig1)};
    for (uint32_t k = 0; k < 2; k++) {
        const uint32_t i = chosen[k];
        out.use[k] = designed[i] && designs[i].estimated_bits < analyses[i].estimated_bits;
        if (out.use[k]) {
            out.candidate[k] = designs[i];
        }
    }
    plan.analysis0 = analyses[chosen[0]];
    plan.analysis1 = analyses[chosen[1]];
    return plan;
}

// price_lpc() out of line, for the stereo search
FLAC_NOINLINE bool price_lpc_alone(const PackedSignal& signal, uint32_t bps,
                                   const SubframeSettings& settings, LpcCandidate& candidate) {
    int32_t work[LPC_WORK_SAMPLES];
    return price_lpc(signal, bps, settings, candidate, work);
}

// Design the four candidates from a finished joint autocorrelation, skipping
// constant ones, which code as CONSTANT
void design_stereo_candidates(LpcStereoAutocorrelation& st, const ChannelAnalysis (&analyses)[4],
                              uint32_t bps, uint32_t precision, LpcCandidate (&designs)[4],
                              bool (&designed)[4]) {
    for (uint32_t i = 0; i < 2; i++) {
        designed[i] = !analyses[i].is_constant && lpc_design((i == 0) ? st.left : st.right, bps,
                                                             precision, designs[i].predictor);
    }
    // Mid's and side's autocorrelations replace left's and right's
    lpc_stereo_autocorrelation_finish(st, st.left, st.right);
    for (uint32_t i = 2; i < 4; i++) {
        designed[i] = !analyses[i].is_constant &&
                      lpc_design((i == 2) ? st.left : st.right, (i == 3) ? bps + 1 : bps, precision,
                                 designs[i].predictor);
    }
}

// The stereo search's walk 1, for all four candidates at once, unpacking
// each chunk once as left and right. Out of line, so its stack is gone before
// the pricing walks.
FLAC_NOINLINE void design_stereo(const PackedSignal& base, const ChannelAnalysis (&analyses)[4],
                                 uint32_t bps, bool wide, const SubframeSettings& settings,
                                 LpcCandidate (&designs)[4], bool (&designed)[4]) {
    const uint32_t n = base.n;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init) -- set by its begin()
    LpcStereoAutocorrelation st;
    int32_t work_left[LPC_WORK_SAMPLES];
    int32_t work_right[LPC_WORK_SAMPLES];
    const uint32_t max_order = settings.lpc_max_order;
    lpc_stereo_autocorrelation_begin(st, n, (max_order < n) ? max_order : (n - 1), bps, wide);
    const size_t frame_bytes = static_cast<size_t>(base.bytes) * 2;
    int32_t* const left = base.buf;
    int32_t* const right = base.buf + base.chunk_len;
    for (uint32_t start = 0; start < n; start += base.chunk_len) {
        const uint32_t m = (n - start < base.chunk_len) ? (n - start) : base.chunk_len;
        unpack_stereo(left, right, base.input + (start * frame_bytes), m, base.bps, base.bytes);
        lpc_stereo_autocorrelation_update(st, left, right, m, work_left, work_right);
    }
    design_stereo_candidates(st, analyses, bps, settings.lpc_precision, designs, designed);
}

// The stereo search: one walk designs all four candidates, then each is
// priced. Candidates are designed one by one instead of jointly when the
// joint sums could overflow (a long wide block) or when any candidate has
// wasted bits (`wasted` is indexed by SignalId).
FLAC_NOINLINE StereoPlan search_stereo_lpc(const PackedSignal& base,
                                           const ChannelAnalysis (&analyses)[4], uint32_t bps,
                                           const SubframeSettings& settings,
                                           const uint32_t (&wasted)[4], StereoLpc& out) {
    LpcCandidate designs[4];
    bool designed[4] = {false, false, false, false};
    const SignalId ids[4] = {SignalId::LEFT, SignalId::RIGHT, SignalId::MID, SignalId::SIDE};
    const uint32_t n = base.n;
    const bool wide = deep_stream(base);
    const bool joint = n >= 2 && (!wide || n <= LPC_STEREO_WIDE_MAX_SAMPLES) &&
                       (wasted[0] | wasted[1] | wasted[2] | wasted[3]) == 0;
    if (joint) {
        design_stereo(base, analyses, bps, wide, settings, designs, designed);
    }
    for (uint32_t i = 0; i < 4; i++) {
        PackedSignal signal = base;
        signal.sig = ids[i];
        signal.wasted = wasted[i];
        const uint32_t signal_bps = ((ids[i] == SignalId::SIDE) ? bps + 1 : bps) - wasted[i];
        if (joint) {
            designed[i] = designed[i] && price_lpc_alone(signal, signal_bps, settings, designs[i]);
        } else {
            designed[i] =
                !analyses[i].is_constant && design_lpc(signal, signal_bps, settings, designs[i]);
        }
    }
    return choose_stereo_plan_with_lpc(analyses, designs, designed, bps, out);
}

// Subframe k's design from the search, or null where FIXED won. Either way
// Pass B designs no LPC of its own.
FLAC_ALWAYS_INLINE const LpcCandidate* searched_design(const StereoLpc& searched, uint32_t k,
                                                       SubframeSettings& settings) {
    settings.lpc_max_order = 0;
    return searched.use[k] ? &searched.candidate[k] : nullptr;
}

#endif  // MICRO_FLAC_ENCODER_DISABLE_LPC

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
constexpr uint8_t FLACEncoder::MAX_LPC_ORDER;
constexpr bool FLACEncoder::LPC_AVAILABLE;
constexpr uint8_t FLACEncoder::MAX_LPC_BITS_PER_SAMPLE;
constexpr uint8_t FLACEncoder::MAX_RICE_PARTITION_ORDER;
constexpr bool FLACEncoder::RICE_PARTITIONS_AVAILABLE;
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

    const FLACEncoderResult result =
        FLAC_UNLIKELY(this->format_.bits_per_sample() > MAX_SCAN32_BITS_PER_SAMPLE)
            ? this->encode_frame64(input, this->options_.block_size, output, output_size_bytes,
                                   bytes_written)
            : this->encode_frame(input, this->options_.block_size, output, output_size_bytes,
                                 bytes_written);
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
            FLAC_UNLIKELY(this->format_.bits_per_sample() > MAX_SCAN32_BITS_PER_SAMPLE)
                ? this->encode_frame64(input, num_samples, output, output_size_bytes, bytes_written)
                : this->encode_frame(input, num_samples, output, output_size_bytes, bytes_written);
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
    this->lpc_max_order_ = 0;
    this->lpc_precision_ = 0;
#ifndef MICRO_FLAC_ENCODER_DISABLE_LPC
    this->lpc_stereo_search_ = false;
#endif
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
    if (options.lpc && (options.max_lpc_order < 1 || options.max_lpc_order > MAX_LPC_ORDER)) {
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
#ifndef MICRO_FLAC_ENCODER_DISABLE_LPC
    if (options.lpc && bits_per_sample <= MAX_LPC_BITS_PER_SAMPLE) {
        this->lpc_max_order_ = options.max_lpc_order;
        this->lpc_precision_ = static_cast<uint8_t>(lpc_precision(bits_per_sample, block_size));
        this->lpc_stereo_search_ = options.lpc_stereo_search && (num_channels == 2);
    }
#endif
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
    const SubframeSettings settings{this->lpc_max_order_, this->lpc_precision_,
                                    this->max_partition_order_};
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
            write_subframe(bw, signal, bps - signal.wasted, analysis, settings, nullptr);
        }

        return this->finish_frame(close_frame(bw, output), num_samples, bytes_written);
    }

    // ---- Stereo Pass A: one walk analyzes all four candidates ----
    //
    // Each chunk is unpacked to [left, right] and scanned, then converted to
    // [mid, side] in place and scanned again. The scans are scoped to Pass A,
    // keeping the stack frame within Xtensa's 1020-byte l32i reach.
    const bool estimate_stereo = this->stereo_estimation_enabled_;
    ChannelAnalysis analyses[4];        // Indexed by SignalId
    uint32_t wasted[4] = {0, 0, 0, 0};  // Indexed by SignalId
    {
        SignalScan scan_l{};
        SignalScan scan_r{};
        SignalScan scan_mid{};
        SignalScan scan_side{};
        uint32_t bits[4] = {0, 0, 0, 0};  // Each candidate's samples ORed, indexed by SignalId
        for (uint32_t start = 0; start < num_samples; start += chunk_len) {
            const uint32_t m =
                (num_samples - start > chunk_len) ? chunk_len : (num_samples - start);
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
        analyses[0] = finish_wasted_analysis(scan_l, num_samples, bps, bits[0], wasted[0]);
        analyses[1] = finish_wasted_analysis(scan_r, num_samples, bps, bits[1], wasted[1]);
        if (estimate_stereo) {
            analyses[2] = finish_wasted_analysis(scan_mid, num_samples, bps, bits[2], wasted[2]);
            analyses[3] =
                finish_wasted_analysis(scan_side, num_samples, side_bps, bits[3], wasted[3]);
        }
    }
    StereoPlan plan{};
    plan.channel_assignment = static_cast<uint8_t>(CHANNEL_INDEPENDENT_STEREO);
    plan.sig0 = SignalId::LEFT;
    plan.bps0 = bps;
    plan.analysis0 = analyses[0];
    plan.sig1 = SignalId::RIGHT;
    plan.bps1 = bps;
    plan.analysis1 = analyses[1];
    SubframeSettings settings0 = settings;
    SubframeSettings settings1 = settings;
    const LpcCandidate* designed0 = nullptr;
    const LpcCandidate* designed1 = nullptr;
#ifndef MICRO_FLAC_ENCODER_DISABLE_LPC
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init) -- read only after the search sets it
    StereoLpc searched;
#endif
    if (estimate_stereo) {
        plan = choose_stereo_plan(analyses[0], analyses[1], analyses[2], analyses[3], bps);
#ifndef MICRO_FLAC_ENCODER_DISABLE_LPC
        if (this->lpc_stereo_search_) {
            plan = search_stereo_lpc(base, analyses, bps, settings, wasted, searched);
            designed0 = searched_design(searched, 0, settings0);
            designed1 = searched_design(searched, 1, settings1);
        }
#endif
    }

    BitWriterLocal bw{};
    const uint8_t header_len = this->start_frame(output, num_samples, plan.channel_assignment);
    writer_init(bw, output + header_len, output_size_bytes - header_len);

    // ---- Stereo Pass B ----
    PackedSignal signal0 = base;
    signal0.sig = plan.sig0;
    signal0.wasted = wasted[static_cast<uint32_t>(plan.sig0)];
    write_subframe(bw, signal0, plan.bps0 - signal0.wasted, plan.analysis0, settings0, designed0);
    PackedSignal signal1 = base;
    signal1.sig = plan.sig1;
    signal1.wasted = wasted[static_cast<uint32_t>(plan.sig1)];
    write_subframe(bw, signal1, plan.bps1 - signal1.wasted, plan.analysis1, settings1, designed1);

    return this->finish_frame(close_frame(bw, output), num_samples, bytes_written);
}

// encode_frame() for streams deeper than MAX_SCAN32_BITS_PER_SAMPLE: the
// 64-bit Pass A, then the same Pass B, with fixed predictors only. Separate,
// so encode_frame()'s tuned loops never see the 64-bit path.
FLAC_COLD_SECTION FLACEncoderResult FLACEncoder::encode_frame64(const uint8_t* input,
                                                                uint32_t num_samples,
                                                                uint8_t* output,
                                                                size_t output_size_bytes,
                                                                size_t& bytes_written) {
    const uint32_t bps = this->format_.bits_per_sample();
    const uint32_t nch = this->format_.num_channels();

    // Each signal of this frame is `base` with its channel or candidate set
    const PackedSignal base{input,          num_samples,  bps,           4, nch, 0,
                            SignalId::LEFT, this->chunk_, CHUNK_SAMPLES, 0};
    const SubframeSettings settings{0, 0, this->max_partition_order_};
    const bool find_wasted = this->options_.wasted_bits;
    BitWriterLocal bw{};

    if (nch != 2) {
        const uint8_t header_len =
            this->start_frame(output, num_samples, static_cast<uint8_t>(nch - 1));
        writer_init(bw, output + header_len, output_size_bytes - header_len);
        for (uint32_t ch = 0; ch < nch; ch++) {
            PackedSignal signal = base;
            signal.channel = ch;
            const ChannelAnalysis analysis = analyze_channel64(signal, find_wasted, signal.wasted);
            write_subframe(bw, signal, bps - signal.wasted, analysis, settings, nullptr);
        }
        return this->finish_frame(close_frame(bw, output), num_samples, bytes_written);
    }

    const bool estimate_stereo = this->stereo_estimation_enabled_;
    ChannelAnalysis analyses[4];        // Indexed by SignalId
    uint32_t wasted[4] = {0, 0, 0, 0};  // Indexed by SignalId
    analyze_stereo64(base, estimate_stereo, find_wasted, analyses, wasted);
    StereoPlan plan{};
    plan.channel_assignment = static_cast<uint8_t>(CHANNEL_INDEPENDENT_STEREO);
    plan.sig0 = SignalId::LEFT;
    plan.bps0 = bps;
    plan.analysis0 = analyses[0];
    plan.sig1 = SignalId::RIGHT;
    plan.bps1 = bps;
    plan.analysis1 = analyses[1];
    if (estimate_stereo) {
        plan = choose_stereo_plan(analyses[0], analyses[1], analyses[2], analyses[3], bps);
    }

    const uint8_t header_len = this->start_frame(output, num_samples, plan.channel_assignment);
    writer_init(bw, output + header_len, output_size_bytes - header_len);
    PackedSignal signal0 = base;
    signal0.sig = plan.sig0;
    signal0.wasted = wasted[static_cast<uint32_t>(plan.sig0)];
    write_subframe(bw, signal0, plan.bps0 - signal0.wasted, plan.analysis0, settings, nullptr);
    PackedSignal signal1 = base;
    signal1.sig = plan.sig1;
    signal1.wasted = wasted[static_cast<uint32_t>(plan.sig1)];
    write_subframe(bw, signal1, plan.bps1 - signal1.wasted, plan.analysis1, settings, nullptr);

    return this->finish_frame(close_frame(bw, output), num_samples, bytes_written);
}
}  // namespace micro_flac
