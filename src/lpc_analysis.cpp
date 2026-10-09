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

#include "lpc_analysis.h"

#ifndef MICRO_FLAC_ENCODER_DISABLE_LPC  // The whole file

#include "compiler.h"
#include "flac_format.h"

#include <cstring>

namespace micro_flac {

namespace {

// ============================================================================
// Window
// ============================================================================

// Tukey(0.5), libFLAC's default window: flat in the middle half, with taper
// weight sin^2(pi/2 * u / taper) at distance u from either edge, taper =
// n/4 - 1. Interpolated from this Q15 table of sin^2(pi/2 * t/64), within 5
// LSB.
constexpr uint32_t WINDOW_ONE = 32768;
constexpr uint32_t WINDOW_Q = 15;
constexpr uint32_t WINDOW_TABLE_STEPS = 64;
constexpr uint16_t WINDOW_TABLE[WINDOW_TABLE_STEPS + 1] = {
    0,     20,    79,    177,   315,   491,   705,   958,   1247,  1573,  1935,  2331,  2761,
    3224,  3719,  4244,  4799,  5381,  5990,  6624,  7282,  7961,  8661,  9379,  10114, 10864,
    11628, 12403, 13188, 13980, 14778, 15580, 16384, 17188, 17990, 18788, 19580, 20365, 21140,
    21904, 22654, 23389, 24107, 24807, 25486, 26144, 26778, 27387, 27969, 28524, 29049, 29544,
    30007, 30437, 30833, 31195, 31521, 31810, 32063, 32277, 32453, 32591, 32689, 32748, 32768,
};

// Window phase is Q24: [0, 2^24] spans the taper, and its top 6 bits index
// the table.
constexpr uint32_t PHASE_BITS = 24;
constexpr uint32_t PHASE_ONE = 1U << PHASE_BITS;
constexpr uint32_t PHASE_INDEX_SHIFT = PHASE_BITS - 6;  // log2(WINDOW_TABLE_STEPS) = 6
constexpr uint32_t PHASE_FRAC_MASK = (1U << PHASE_INDEX_SHIFT) - 1;

// Taper weight at distance u from the edge, Q15, at most 32767 so a windowed
// 17-bit sample's product stays in int32. Only the table's end needs the cap:
// its last step rises from 32748 by under 20.
FLAC_ALWAYS_INLINE int32_t taper_weight(uint32_t u, uint32_t step) {
    // u <= taper < 2^14 and step <= 2^24 / taper + 1/2, so the phase is at
    // most 2^24 + 2^13: index 64 at the very end, never past it.
    const uint32_t phase = u * step;
    const uint32_t index = phase >> PHASE_INDEX_SHIFT;
    if (index >= WINDOW_TABLE_STEPS) {
        return static_cast<int32_t>(WINDOW_ONE) - 1;
    }
    const int32_t lo = WINDOW_TABLE[index];
    const int32_t hi = WINDOW_TABLE[index + 1];
    const int32_t frac = static_cast<int32_t>(phase & PHASE_FRAC_MASK);
    // hi - lo < 2^10 and frac < 2^18: the product fits.
    return lo + (((hi - lo) * frac) >> PHASE_INDEX_SHIFT);
}

// Windowed samples are kept within +/-32767, so two of their products sum
// in int32 (see correlate()).
constexpr int32_t WINDOWED_MAX = 32767;

FLAC_ALWAYS_INLINE int32_t clamp_windowed(int32_t v) {
    v = (v > WINDOWED_MAX) ? WINDOWED_MAX : v;
    return (v < -WINDOWED_MAX) ? -WINDOWED_MAX : v;
}

// Window `len` samples from ac.position into `w`, scaled down by
// ac.scale_shift and clamped to +/-WINDOWED_MAX, in rising-taper, flat and
// falling-taper runs so the flat middle costs a shift and a clamp per sample.
// A 17-bit side sample is halved, which costs nothing measurable. The clamp
// acts only in the flat run, on -32768 at 16 bits and rounded-up 65535 at 17;
// a taper's weight keeps it in range, and its product under 2^31.
//
// The fields are copied to locals so stores to `w` do not force reloads.
void window_piece(const LpcAutocorrelation& ac, const int32_t* x, uint32_t len, int32_t* w) {
    const uint32_t t0 = ac.position;
    const uint32_t n = ac.n;
    const uint32_t step = ac.step;
    const uint32_t scale_shift = ac.scale_shift;
    const uint32_t shift = WINDOW_Q + scale_shift;
    const int32_t round = int32_t{1} << (shift - 1);
    uint32_t rise_end = 0;      // Piece positions below this are in the rising taper,
    uint32_t fall_begin = len;  // and from this one on in the falling taper
    if (ac.taper != 0) {
        const uint32_t rise_limit = ac.taper + 1;
        const uint32_t fall_limit = n - 1 - ac.taper;
        rise_end = (t0 >= rise_limit) ? 0 : ((rise_limit - t0 < len) ? (rise_limit - t0) : len);
        fall_begin = (t0 >= fall_limit) ? 0 : ((fall_limit - t0 < len) ? (fall_limit - t0) : len);
        fall_begin = (fall_begin < rise_end) ? rise_end : fall_begin;
    }
    uint32_t i = 0;
    for (; i < rise_end; i++) {
        w[i] = ((x[i] * taper_weight(t0 + i, step)) + round) >> shift;
    }
    if (scale_shift == 0) {
        for (; i < fall_begin; i++) {
            w[i] = clamp_windowed(x[i]);
        }
    } else {
        const int32_t flat_round = int32_t{1} << (scale_shift - 1);
        for (; i < fall_begin; i++) {
            w[i] = clamp_windowed((x[i] + flat_round) >> scale_shift);
        }
    }
    for (; i < len; i++) {
        w[i] = ((x[i] * taper_weight(n - 1 - (t0 + i), step)) + round) >> shift;
    }
}

// Sum of w[i] * w[i - lag] over the piece's `len` samples. Each product of
// two samples within +/-32767 is under 2^30, so a pair of them sums in
// int32, and only every other product pays for the 64-bit add (which
// Xtensa builds from 32-bit halves with a carry).
FLAC_ALWAYS_INLINE int64_t correlate(const int32_t* w, uint32_t len, uint32_t lag) {
    const int32_t* partner = w - lag;
    int64_t acc = 0;
    uint32_t i = 0;
    for (; i + 1 < len; i += 2) {
        acc += (w[i] * partner[i]) + (w[i + 1] * partner[i + 1]);
    }
    if (i < len) {
        acc += static_cast<int32_t>(w[i] * partner[i]);
    }
    return acc;
}

// correlate() at lags `lag` and `lag + 1` in one walk. The two share every
// load of w[i], and lag + 1's partner for sample i + 1 is lag's partner for
// sample i, so each step loads four values for four products instead of
// eight. lag + 1 <= LPC_MAX_ORDER, so every partner is in the piece or the
// history before it.
FLAC_ALWAYS_INLINE void correlate_pair(const int32_t* w, uint32_t len, uint32_t lag, int64_t& sum0,
                                       int64_t& sum1) {
    const int32_t* partner = w - lag;
    int32_t before = partner[-1];  // w[i - lag - 1] for the step's first sample
    int64_t acc0 = 0;
    int64_t acc1 = 0;
    uint32_t i = 0;
    for (; i + 1 < len; i += 2) {
        const int32_t a0 = w[i];
        const int32_t a1 = w[i + 1];
        const int32_t b0 = partner[i];
        const int32_t b1 = partner[i + 1];
        acc0 += (a0 * b0) + (a1 * b1);
        acc1 += (a0 * before) + (a1 * b0);
        before = b1;
    }
    if (i < len) {
        // Products under 2^30 (see above), so int32 multiplies on purpose
        acc0 += static_cast<int32_t>(w[i] * partner[i]);
        acc1 += static_cast<int32_t>(w[i] * before);
    }
    sum0 += acc0;
    sum1 += acc1;
}

// The wide path keeps windowed samples within +/-(2^23 - 1): full precision
// for a 24-bit stream (its 25-bit side channel gives up one bit). A product
// is then under 2^46, and a signal of at most 65535 samples sums under 2^62
// at every lag, so each product takes a plain 64-bit multiply-add.
constexpr int32_t WIDE_WINDOWED_MAX = (int32_t{1} << 23) - 1;
constexpr uint32_t WIDE_WINDOWED_BPS = 24;

FLAC_ALWAYS_INLINE int32_t clamp_wide(int64_t v) {
    v = (v > WIDE_WINDOWED_MAX) ? WIDE_WINDOWED_MAX : v;
    return static_cast<int32_t>((v < -WIDE_WINDOWED_MAX) ? -WIDE_WINDOWED_MAX : v);
}

// The wide path's taper weight, Q31, within 2 LSB. Q15 weights' rounding
// puts a floor under the windowed spectrum that a deep, oversampled signal's
// prediction error sits far below. sin(z) is its Taylor series through z^11
// where z <= pi/4, and 1 - sin^2(pi/2 - z) beyond, on prescaled Q31
// coefficients so no step divides.
constexpr uint32_t WIDE_WEIGHT_Q = 31;
constexpr int64_t WIDE_WEIGHT_ONE = int64_t{1} << WIDE_WEIGHT_Q;
constexpr int64_t WIDE_WEIGHT_ROUND = int64_t{1} << (WIDE_WEIGHT_Q - 1);
// pi/2 in Q47, so z = u * (this / taper) >> 16 is Q31 with an error of
// about 2^-32 for any u <= taper < 2^16.
constexpr uint64_t WIDE_HALF_PI_Q47 = 221069929750889ULL;

// A weight's z step for the piece's taper: pi/2 / taper in Q47.
FLAC_ALWAYS_INLINE uint64_t wide_weight_step(uint32_t taper) {
    return (WIDE_HALF_PI_Q47 + (taper / 2)) / taper;
}

// sin(z) = z + z^3 * P(z^2), with P's coefficients (-1/3!, 1/5!, -1/7!,
// 1/9!, -1/11!) in Q31, listed innermost first for Horner's rule.
constexpr int64_t WIDE_SINE_COEFFS[] = {-54, 5918, -426088, 17895697, -357913941};

FLAC_ALWAYS_INLINE int64_t wide_taper_weight(uint32_t u, uint32_t taper, uint64_t step) {
    const bool upper = (2 * u) > taper;
    const uint32_t v = upper ? (taper - u) : u;
    // z <= pi/4 < 2^31 in Q31; every product below is under 2^62.
    const int64_t z = static_cast<int64_t>(((v * step) + (uint64_t{1} << 15)) >> 16);
    const int64_t z2 = ((z * z) + WIDE_WEIGHT_ROUND) >> WIDE_WEIGHT_Q;
    int64_t p = WIDE_SINE_COEFFS[0];
    for (uint32_t i = 1; i < 5; i++) {
        p = WIDE_SINE_COEFFS[i] + (((z2 * p) + WIDE_WEIGHT_ROUND) >> WIDE_WEIGHT_Q);
    }
    const int64_t z3 = ((z * z2) + WIDE_WEIGHT_ROUND) >> WIDE_WEIGHT_Q;
    const int64_t sine = z + (((z3 * p) + WIDE_WEIGHT_ROUND) >> WIDE_WEIGHT_Q);
    const int64_t square = ((sine * sine) + WIDE_WEIGHT_ROUND) >> WIDE_WEIGHT_Q;
    return upper ? (WIDE_WEIGHT_ONE - square) : square;
}

// window_piece() for the wide path: a sample of up to 25 bits times a Q31
// weight, in 64 bits, split into the same rising-taper, flat and
// falling-taper runs. The taper runs stop short of the weight reaching 1.0
// (u == taper), which the flat run's arithmetic gives exactly.
void window_piece_wide(const LpcAutocorrelation& ac, const int32_t* x, uint32_t len, int32_t* w) {
    const uint32_t t0 = ac.position;
    const uint32_t n = ac.n;
    const uint32_t taper = ac.taper;
    const uint32_t scale_shift = ac.scale_shift;
    const uint32_t shift = WIDE_WEIGHT_Q + scale_shift;
    const int64_t round = int64_t{1} << (shift - 1);
    const int64_t flat_round = (scale_shift != 0) ? (int64_t{1} << (scale_shift - 1)) : 0;
    uint32_t rise_end = 0;      // Piece positions below this are in the rising taper,
    uint32_t fall_begin = len;  // and from this one on in the falling taper
    uint64_t step = 0;
    if (taper != 0) {
        step = wide_weight_step(taper);
        rise_end = (t0 >= taper) ? 0 : ((taper - t0 < len) ? (taper - t0) : len);
        const uint32_t fall_limit = n - taper;
        fall_begin = (t0 >= fall_limit) ? 0 : ((fall_limit - t0 < len) ? (fall_limit - t0) : len);
        fall_begin = (fall_begin < rise_end) ? rise_end : fall_begin;
    }
    uint32_t i = 0;
    for (; i < rise_end; i++) {
        const int64_t weight = wide_taper_weight(t0 + i, taper, step);
        w[i] = clamp_wide(((int64_t{x[i]} * weight) + round) >> shift);
    }
    for (; i < fall_begin; i++) {
        w[i] = clamp_wide((int64_t{x[i]} + flat_round) >> scale_shift);
    }
    for (; i < len; i++) {
        const int64_t weight = wide_taper_weight(n - 1 - (t0 + i), taper, step);
        w[i] = clamp_wide(((int64_t{x[i]} * weight) + round) >> shift);
    }
}

// Every lag's sum over a windowed piece, in 64 bits (see WIDE_WINDOWED_MAX).
void correlate_wide(const int32_t* w, uint32_t len, uint32_t lags, int64_t* sums) {
    for (uint32_t l = 0; l <= lags; l++) {
        const int32_t* partner = w - l;
        int64_t acc = 0;
        for (uint32_t i = 0; i < len; i++) {
            acc += static_cast<int64_t>(w[i]) * partner[i];
        }
        sums[l] += acc;
    }
}

// lpc_autocorrelation_update() on the wide path, piece by piece like the
// narrow one. Out of line and entered once per call: inlined into the narrow
// update, its 64-bit code cost the narrow correlation loop a register spill,
// 2-4% on 16-bit LPC encodes on the ESP32-S3.
FLAC_NOINLINE void autocorrelation_update_wide(LpcAutocorrelation& ac, const int32_t* samples,
                                               uint32_t m, int32_t* work) {
    int32_t* const w = work + LPC_MAX_ORDER;
    while (m > 0) {
        const uint32_t len = (m > LPC_KERNEL_SAMPLES) ? LPC_KERNEL_SAMPLES : m;
        std::memcpy(work, ac.history, sizeof(ac.history));
        window_piece_wide(ac, samples, len, w);
        correlate_wide(w, len, ac.lags, ac.sum);
        std::memcpy(ac.history, work + len, sizeof(ac.history));
        ac.position += len;
        samples += len;
        m -= len;
    }
}

// ============================================================================
// Fixed-point helpers
// ============================================================================

uint32_t floor_log2(uint64_t v) {
    return 63U - static_cast<uint32_t>(__builtin_clzll(v));  // NOLINT(readability-magic-numbers)
}

// log2(v) in Q16 for v > 0: the MSB position plus a quadratic fit of
// log2(1 + f) over the mantissa f (error under 0.005, which only ever
// matters to order selection's ranking).
int64_t log2_q16(uint64_t v) {
    const uint32_t msb = floor_log2(v);
    const uint64_t f = (msb >= 16) ? ((v >> (msb - 16)) & 0xFFFFU) : ((v << (16 - msb)) & 0xFFFFU);
    // log2(1 + f) ~= f + 0.3431 * f * (1 - f)
    constexpr int64_t FIT_Q16 = 22486;
    const int64_t fi = static_cast<int64_t>(f);
    const int64_t correction = (((fi * (65536 - fi)) >> 16) * FIT_Q16) >> 16;
    return (static_cast<int64_t>(msb) << 16) + fi + correction;
}

// Reflection coefficients are Q30, so |rc| < 1 means |rc| < 2^30.
constexpr int RC_Q = 30;
constexpr int64_t RC_ONE = int64_t{1} << RC_Q;

// Direct-form predictor coefficients are Q20 in int64. A predictor built
// from reflection coefficients of magnitude below 1 has |a_j| <= C(order, j)
// <= C(12, 6) = 924 < 2^10, so every coefficient stays under 2^30 and every
// product with a reflection coefficient under 2^60.
constexpr int COEF_Q = 20;

// The normalized autocorrelation's R[0] sits in [2^29, 2^30).
constexpr uint32_t NORM_MSB = 29;

// Schur's intermediates stay within R[0] in exact arithmetic; this bound
// (twice the largest R[0]) is enforced so that rounding can never push a
// product past 64 bits.
constexpr int64_t SCHUR_LIMIT = int64_t{1} << 31;

// The wide path normalizes R[0] into [2^45, 2^46) and carries reflection
// coefficients in Q40. Deep, oversampled signals put high orders' reflection
// coefficients within a hair of 1, where the direct-form predictor is very
// sensitive to them; the narrow path's precision designs them badly.
constexpr uint32_t WIDE_NORM_MSB = 45;
constexpr int64_t WIDE_SCHUR_LIMIT = int64_t{1} << 47;
constexpr int WIDE_RC_Q = 40;
constexpr int64_t WIDE_RC_ONE = int64_t{1} << WIDE_RC_Q;

// (a * b) >> shift, truncated toward zero, for |a|, |b| < 2^63 and 0 < shift
// < 64 with a result under 2^63: the 128-bit product assembled from 32-bit
// halves, since the wide path's products run to about 2^87.
FLAC_ALWAYS_INLINE int64_t mul_shift_wide(int64_t a, int64_t b, uint32_t shift) {
    const bool negative = (a < 0) != (b < 0);
    const uint64_t x = static_cast<uint64_t>((a < 0) ? -a : a);
    const uint64_t y = static_cast<uint64_t>((b < 0) ? -b : b);
    const uint64_t x_lo = x & 0xFFFFFFFFU;
    const uint64_t x_hi = x >> 32;
    const uint64_t y_lo = y & 0xFFFFFFFFU;
    const uint64_t y_hi = y >> 32;
    const uint64_t p0 = x_lo * y_lo;
    const uint64_t p1 = x_lo * y_hi;
    const uint64_t p2 = x_hi * y_lo;
    const uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFFU) + (p2 & 0xFFFFFFFFU);
    const uint64_t lo = (p0 & 0xFFFFFFFFU) | (mid << 32);
    const uint64_t hi = (x_hi * y_hi) + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
    const uint64_t magnitude = (hi << (64 - shift)) | (lo >> shift);
    return negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
}

// (num << WIDE_RC_Q) / den, truncated toward zero, for |num| < 2^47 and
// 0 < den < 2^47, by long division 15 bits at a time (each shifted remainder
// stays under 2^62). |num| >= den, which no positive definite recursion
// produces, returns a magnitude of WIDE_RC_ONE for the caller to reject.
FLAC_ALWAYS_INLINE int64_t div_rc_wide(int64_t num, int64_t den) {
    const uint64_t n = static_cast<uint64_t>((num < 0) ? -num : num);
    const uint64_t d = static_cast<uint64_t>(den);
    uint64_t q = 0;
    if (n >= d) {
        q = static_cast<uint64_t>(WIDE_RC_ONE);
    } else {
        uint64_t rem = n;
        for (uint32_t left = WIDE_RC_Q; left > 0;) {
            const uint32_t step = (left < 15) ? left : 15;
            rem <<= step;
            q = (q << step) | (rem / d);
            rem %= d;
            left -= step;
        }
    }
    return (num < 0) ? -static_cast<int64_t>(q) : static_cast<int64_t>(q);
}

// The design's reflection-coefficient arithmetic on each path: a product
// with a reflection coefficient, (r * t) >> 30 (>> 40 wide), and a
// reflection coefficient, -(num << 30) / den (<< 40 wide).
FLAC_ALWAYS_INLINE int64_t rc_mul(int64_t r, int64_t t, bool wide) {
    if (wide) {
        return mul_shift_wide(r, t, WIDE_RC_Q);
    }
    return (r * t) >> RC_Q;
}

FLAC_ALWAYS_INLINE int64_t schur_reflection(int64_t num, int64_t den, bool wide) {
    if (wide) {
        return -div_rc_wide(num, den);
    }
    return -((num * RC_ONE) / den);
}

// libFLAC's default coefficient precision cap for the residual dot product:
// bps + precision + floor(log2(order)) <= 32 keeps order products of a
// (precision)-bit coefficient and a (bps)-bit sample, and their sum, inside
// int32 (each term is under 2^(bps + precision - 2), and there are fewer
// than 2^(floor(log2(order)) + 1) of them). The wide path's 64-bit dot
// product needs no cap: 25 + 15 + 4 bits is far inside 64.
uint32_t precision_for(uint32_t precision, uint32_t bps, uint32_t order, bool wide) {
    uint32_t p = precision;
    if (!wide) {
        const uint32_t cap = 32 - bps - floor_log2(order);
        p = (p < cap) ? p : cap;
    }
    return (p > LPC_PRECISION_MAX) ? LPC_PRECISION_MAX : p;
}

}  // namespace

// ============================================================================
// Autocorrelation
// ============================================================================

// bps <= LPC_MAX_SUBFRAME_BPS. Up to 17 bits, window_piece()'s int32 bounds
// hold; deeper signals take the wide path.
void lpc_autocorrelation_begin(LpcAutocorrelation& ac, uint32_t n, uint32_t lags, uint32_t bps) {
    for (int64_t& s : ac.sum) {
        s = 0;
    }
    for (int32_t& h : ac.history) {
        h = 0;
    }
    ac.lags = lags;
    ac.n = n;
    ac.position = 0;
    // libFLAC: Np = (int)(p / 2 * n) - 1 with p = 0.5, and no window unless
    // Np > 0.
    const uint32_t quarter = n / 4;
    ac.taper = (quarter >= 2) ? (quarter - 1) : 0;
    ac.step = (ac.taper != 0) ? ((PHASE_ONE + (ac.taper / 2)) / ac.taper) : 0;
    ac.scale_shift = (bps > 16) ? (bps - 16) : 0;
    ac.wide = false;
    if (bps > LPC_NARROW_MAX_SUBFRAME_BPS) {
        ac.wide = true;
        ac.scale_shift = (bps > WIDE_WINDOWED_BPS) ? (bps - WIDE_WINDOWED_BPS) : 0;
    }
}

void lpc_autocorrelation_begin_wide(LpcAutocorrelation& ac, uint32_t n, uint32_t lags,
                                    uint32_t bps) {
    lpc_autocorrelation_begin(ac, n, lags, bps);
    ac.wide = true;
    ac.scale_shift = (bps > WIDE_WINDOWED_BPS) ? (bps - WIDE_WINDOWED_BPS) : 0;
}

void lpc_autocorrelation_update(LpcAutocorrelation& ac, const int32_t* samples, uint32_t m,
                                int32_t* work) {
    if (ac.wide) {
        autocorrelation_update_wide(ac, samples, m, work);
        return;
    }
    const uint32_t lags = ac.lags;
    int32_t* const w = work + LPC_MAX_ORDER;
    while (m > 0) {
        const uint32_t len = (m > LPC_KERNEL_SAMPLES) ? LPC_KERNEL_SAMPLES : m;
        // work = [the previous LPC_MAX_ORDER windowed samples, this piece
        // windowed], so every lag reads its partner by plain indexing. Before
        // the first sample the history is zero, which contributes nothing.
        std::memcpy(work, ac.history, sizeof(ac.history));
        window_piece(ac, samples, len, w);
        // Every product is under 2^30 and a signal has at most 65535
        // samples, so every lag's sum stays under 2^46.
        uint32_t l = 0;
        for (; l + 1 <= lags; l += 2) {
            correlate_pair(w, len, l, ac.sum[l], ac.sum[l + 1]);
        }
        if (l <= lags) {
            ac.sum[l] += correlate(w, len, l);
        }
        std::memcpy(ac.history, work + len, sizeof(ac.history));
        ac.position += len;
        samples += len;
        m -= len;
    }
}

// ============================================================================
// Predictor design
// ============================================================================

bool lpc_design(const LpcAutocorrelation& ac, uint32_t bps, uint32_t precision, LpcPredictor& out) {
    const uint32_t lags = ac.lags;
    const int64_t r0 = ac.sum[0];
    if (lags == 0 || r0 <= 0) {
        return false;  // Silence under the window
    }

    // Normalize so R[0] sits in [2^29, 2^30) ([2^45, 2^46) on the wide
    // path). |R[l]| <= R[0] for every lag (Cauchy-Schwarz), so all of them
    // fit too. The true values are the normalized ones times 2^norm_log2.
    const bool wide = ac.wide;
    uint32_t norm_msb = NORM_MSB;
    int64_t schur_limit = SCHUR_LIMIT;
    int64_t rc_one = RC_ONE;
    int rc_q = RC_Q;
    if (wide) {
        norm_msb = WIDE_NORM_MSB;
        schur_limit = WIDE_SCHUR_LIMIT;
        rc_one = WIDE_RC_ONE;
        rc_q = WIDE_RC_Q;
    }
    const uint32_t msb = floor_log2(static_cast<uint64_t>(r0));
    const int32_t norm_log2 = static_cast<int32_t>(msb) - static_cast<int32_t>(norm_msb);
    int64_t c0[LPC_MAX_ORDER + 1];
    int64_t c1[LPC_MAX_ORDER + 1];
    for (uint32_t l = 0; l <= lags; l++) {
        const int64_t v =
            (norm_log2 >= 0) ? (ac.sum[l] >> norm_log2) : (ac.sum[l] * (int64_t{1} << -norm_log2));
        c0[l] = v;
        c1[l] = v;
    }

    // ---- Schur recursion: reflection coefficients and each order's
    // prediction error, from the autocorrelation alone (the fixed-point
    // form of Levinson-Durbin used by speech codecs such as SILK, whose
    // intermediates stay bounded by R[0]) ----
    int64_t rc[LPC_MAX_ORDER];
    int64_t error[LPC_MAX_ORDER];
    uint32_t orders = 0;
    for (uint32_t k = 0; k < lags; k++) {
        const int64_t e = c1[0];
        if (e <= 0) {
            break;
        }
        // |c0| < 2^31, so the narrow numerator stays under 2^61.
        const int64_t r = schur_reflection(c0[k + 1], e, wide);
        if (r >= rc_one || r <= -rc_one) {
            break;  // Not positive definite at this order (rounding): stop
        }
        bool bounded = true;
        for (uint32_t i = 0; i < lags - k; i++) {
            const int64_t t1 = c0[i + k + 1];
            const int64_t t2 = c1[i];
            c0[i + k + 1] = t1 + rc_mul(r, t2, wide);
            c1[i] = t2 + rc_mul(r, t1, wide);
            bounded = bounded && c0[i + k + 1] < schur_limit && c0[i + k + 1] > -schur_limit &&
                      c1[i] < schur_limit && c1[i] > -schur_limit;
        }
        if (!bounded || c1[0] < 0) {
            break;
        }
        rc[k] = r;
        error[k] = c1[0];
        orders = k + 1;
        if (c1[0] == 0) {
            break;  // Perfectly predicted at this order
        }
    }
    if (orders == 0) {
        return false;
    }

    // ---- Order selection: libFLAC's FLAC__lpc_compute_best_order(), in
    // Q16. A residual costs about 0.5 * log2(error * 0.5 / n) bits (never
    // below 0), and each order adds its warmup sample and coefficient. The
    // error is in the windowed samples' scale; a 17-bit signal's were halved
    // (ac.scale_shift), which quarters the error, so that is added back ----
    const uint32_t n = ac.n;
    const int64_t log2_n = log2_q16(n);
    const int64_t error_log2 =
        static_cast<int64_t>(norm_log2) + (2 * static_cast<int64_t>(ac.scale_shift));
    uint32_t order = 1;
    int64_t best_bits = INT64_MAX;
    for (uint32_t o = 1; o <= orders; o++) {
        int64_t per_residual = 0;
        if (error[o - 1] > 0) {
            const int64_t l2 = log2_q16(static_cast<uint64_t>(error[o - 1])) +
                               (error_log2 * 65536) - (int64_t{1} << 16) - log2_n;
            per_residual = (l2 > 0) ? (l2 / 2) : 0;
        }
        const int64_t overhead =
            static_cast<int64_t>(o) * (bps + precision_for(precision, bps, o, wide));
        const int64_t bits = (per_residual * (n - o)) + (overhead << 16);
        if (bits < best_bits) {
            best_bits = bits;
            order = o;
        }
    }

    // ---- Step-up: reflection coefficients to direct-form predictor
    // coefficients (SILK's silk_k2a()), Q20. a[j] multiplies x[i - 1 - j] ----
    int64_t a[LPC_MAX_ORDER] = {};
    for (uint32_t k = 0; k < order; k++) {
        int64_t prev[LPC_MAX_ORDER];
        for (uint32_t i = 0; i < k; i++) {
            prev[i] = a[i];
        }
        for (uint32_t i = 0; i < k; i++) {
            a[i] = prev[i] + rc_mul(rc[k], prev[k - 1 - i], wide);
        }
        a[k] = -(rc[k] >> (rc_q - COEF_Q));
    }

    // ---- Quantization: libFLAC's FLAC__lpc_quantize_coefficients(), in
    // integers. The shift puts the largest coefficient at the top of the
    // precision; rounding error is carried forward so it cancels across
    // coefficients ----
    const uint32_t coef_precision = precision_for(precision, bps, order, wide);
    const uint32_t magnitude_bits = coef_precision - 1;  // One bit for the sign
    const int64_t qmax = (int64_t{1} << magnitude_bits) - 1;
    const int64_t qmin = -(int64_t{1} << magnitude_bits);
    uint64_t cmax = 0;
    for (uint32_t j = 0; j < order; j++) {
        const uint64_t mag = static_cast<uint64_t>((a[j] < 0) ? -a[j] : a[j]);
        cmax = (mag > cmax) ? mag : cmax;
    }
    if (cmax == 0) {
        return false;
    }
    // floor(log2(cmax as a real number)): frexp()'s exponent minus one.
    const int32_t log2_cmax = static_cast<int32_t>(floor_log2(cmax)) - COEF_Q;
    int32_t shift = static_cast<int32_t>(magnitude_bits) - log2_cmax - 1;
    if (shift > LPC_SHIFT_MAX) {
        shift = LPC_SHIFT_MAX;
    }
    constexpr int64_t HALF = int64_t{1} << (COEF_Q - 1);
    int64_t carried = 0;  // Q20
    for (uint32_t j = 0; j < order; j++) {
        // |a| < 2^30 and shift <= 15: under 2^45. A negative shift (a
        // coefficient too large for the precision) scales down instead and
        // writes shift 0, as libFLAC does.
        carried += (shift >= 0) ? (a[j] * (int64_t{1} << shift)) : (a[j] >> -shift);
        int64_t v = (carried >= 0) ? ((carried + HALF) >> COEF_Q) : -((-carried + HALF) >> COEF_Q);
        v = (v > qmax) ? qmax : ((v < qmin) ? qmin : v);
        carried -= v * (int64_t{1} << COEF_Q);
        out.coefficients[j] = static_cast<int32_t>(v);
    }
    for (uint32_t j = order; j < LPC_MAX_ORDER; j++) {
        out.coefficients[j] = 0;  // The residual kernels read a padded 4, 8 or 12
    }
    out.order = order;
    out.precision = coef_precision;
    out.shift = static_cast<uint32_t>((shift < 0) ? 0 : shift);
    out.wide = wide;
    return true;
}

// ============================================================================
// Residuals
// ============================================================================

namespace {

// |v| as unsigned, in the compare-and-select form GCC lowers to Xtensa's
// one-instruction ABS (see flac_encoder.cpp's abs_u32()).
FLAC_ALWAYS_INLINE uint32_t abs_magnitude(int32_t v) {
    return (v < 0) ? (0U - static_cast<uint32_t>(v)) : static_cast<uint32_t>(v);
}

// The predictor's dot product at `at` (a residual's own sample; its
// predecessors sit below it), over TERMS coefficients: the order rounded up
// to 4, 8 or 12, with the coefficients past the order zero. Unrolled, so each
// term is a load, a multiply and an add. In int32 by lpc_design()'s precision
// cap (see precision_for()); the zero terms add nothing.
template <uint32_t TERMS>
FLAC_ALWAYS_INLINE int32_t predict(const int32_t* c, const int32_t* at) {
    int32_t sum = 0;
    FLAC_UNROLL(12)
    for (uint32_t j = 0; j < TERMS; j++) {
        sum += c[j] * at[-1 - static_cast<int32_t>(j)];
    }
    return sum;
}

// x[i] - (prediction >> shift), in wrapping 32-bit arithmetic. Under
// precision_for()'s cap the true residual always fits int32 (|prediction| <=
// 2^31 - 2^(precision + bps - 2) and |x| < 2^(precision + bps - 2)), so it is
// exact; the unsigned form only keeps that reasoning out of the compiler's
// hands, since a signed overflow would be undefined.
FLAC_ALWAYS_INLINE int32_t residual(int32_t x, int32_t prediction, uint32_t shift) {
    return static_cast<int32_t>(static_cast<uint32_t>(x) -
                                static_cast<uint32_t>(prediction >> shift));
}

template <uint32_t TERMS>
uint32_t sum_terms(const LpcPredictor& predictor, const int32_t* x, uint32_t first, uint32_t m,
                   bool& in_range) {
    const int32_t* const c = predictor.coefficients;
    const uint32_t shift = predictor.shift;
    uint32_t total = 0;
    uint32_t seen = 0;  // OR of every magnitude: at least LIMIT iff one of them is
    for (uint32_t i = first; i < m; i++) {
        const uint32_t magnitude = abs_magnitude(residual(x[i], predict<TERMS>(c, x + i), shift));
        total += magnitude;
        seen |= magnitude;
    }
    in_range = in_range && (seen < static_cast<uint32_t>(LPC_RESIDUAL_LIMIT));
    return total;
}

template <uint32_t TERMS>
void residuals_in_place(const LpcPredictor& predictor, int32_t* x, uint32_t first, uint32_t m) {
    const int32_t* const c = predictor.coefficients;
    const uint32_t shift = predictor.shift;
    // Last first: each residual reads only samples below it. Two at a time
    // spills the coefficients on Xtensa.
    for (uint32_t i = m; i-- > first;) {
        x[i] = residual(x[i], predict<TERMS>(c, x + i), shift);
    }
}

// predict<TERMS>() in 64 bits: a 25-bit sample times a 15-bit coefficient,
// twelve times over, is under 2^44.
template <uint32_t TERMS>
FLAC_ALWAYS_INLINE int64_t predict_wide(const int32_t* c, const int32_t* at) {
    int64_t sum = 0;
    FLAC_UNROLL(12)
    for (uint32_t j = 0; j < TERMS; j++) {
        sum += static_cast<int64_t>(c[j]) * at[-1 - static_cast<int32_t>(j)];
    }
    return sum;
}

template <uint32_t TERMS>
uint64_t sum_terms_wide(const LpcPredictor& predictor, const int32_t* x, uint32_t first, uint32_t m,
                        bool& in_range) {
    const int32_t* const c = predictor.coefficients;
    const uint32_t shift = predictor.shift;
    uint64_t total = 0;
    uint64_t seen = 0;  // OR of every magnitude: at least LIMIT iff one of them is
    for (uint32_t i = first; i < m; i++) {
        const int64_t r = x[i] - (predict_wide<TERMS>(c, x + i) >> shift);
        const uint64_t magnitude = static_cast<uint64_t>((r < 0) ? -r : r);
        total += magnitude;
        seen |= magnitude;
    }
    in_range = in_range && (seen < static_cast<uint64_t>(LPC_WIDE_RESIDUAL_LIMIT));
    return total;
}

// residuals_in_place() in 64 bits. Every residual is below
// LPC_WIDE_RESIDUAL_LIMIT (sum_terms_wide() has checked the walk), so it
// fits the int32 it is stored in.
template <uint32_t TERMS>
void residuals_in_place_wide(const LpcPredictor& predictor, int32_t* x, uint32_t first,
                             uint32_t m) {
    const int32_t* const c = predictor.coefficients;
    const uint32_t shift = predictor.shift;
    for (uint32_t i = m; i-- > first;) {
        x[i] = static_cast<int32_t>(x[i] - (predict_wide<TERMS>(c, x + i) >> shift));
    }
}

// Load a piece behind the walk's history: work = [the previous
// LPC_MAX_ORDER samples, the piece]. Returns how many of the piece's samples
// are warmup (the signal's first `order`), which have no residuals, and
// advances the walk.
uint32_t load_piece(const LpcPredictor& predictor, LpcResidualState& state, const int32_t* samples,
                    uint32_t m, int32_t* work) {
    std::memcpy(work, state.history, sizeof(state.history));
    std::memcpy(work + LPC_MAX_ORDER, samples, m * sizeof(int32_t));
    std::memcpy(state.history, work + m, sizeof(state.history));
    uint32_t first = 0;
    if (state.position < predictor.order) {
        first = predictor.order - state.position;
        first = (first > m) ? m : first;
    }
    state.position += m;
    return first;
}

}  // namespace

uint64_t lpc_residual_sum(const LpcPredictor& predictor, LpcResidualState& state,
                          const int32_t* samples, uint32_t m, int32_t* work) {
    const uint32_t first = load_piece(predictor, state, samples, m, work);
    const int32_t* const x = work + LPC_MAX_ORDER;
    if (predictor.wide) {
        if (predictor.order <= 4) {
            return sum_terms_wide<4>(predictor, x, first, m, state.in_range);
        }
        if (predictor.order <= 8) {
            return sum_terms_wide<8>(predictor, x, first, m, state.in_range);
        }
        return sum_terms_wide<LPC_MAX_ORDER>(predictor, x, first, m, state.in_range);
    }
    if (predictor.order <= 4) {
        return sum_terms<4>(predictor, x, first, m, state.in_range);
    }
    if (predictor.order <= 8) {
        return sum_terms<8>(predictor, x, first, m, state.in_range);
    }
    return sum_terms<LPC_MAX_ORDER>(predictor, x, first, m, state.in_range);
}

const int32_t* lpc_residuals(const LpcPredictor& predictor, LpcResidualState& state,
                             const int32_t* samples, uint32_t m, int32_t* work, uint32_t& count) {
    const uint32_t first = load_piece(predictor, state, samples, m, work);
    int32_t* const x = work + LPC_MAX_ORDER;
    if (predictor.wide) {
        if (predictor.order <= 4) {
            residuals_in_place_wide<4>(predictor, x, first, m);
        } else if (predictor.order <= 8) {
            residuals_in_place_wide<8>(predictor, x, first, m);
        } else {
            residuals_in_place_wide<LPC_MAX_ORDER>(predictor, x, first, m);
        }
        count = m - first;
        return x + first;
    }
    if (predictor.order <= 4) {
        residuals_in_place<4>(predictor, x, first, m);
    } else if (predictor.order <= 8) {
        residuals_in_place<8>(predictor, x, first, m);
    } else {
        residuals_in_place<LPC_MAX_ORDER>(predictor, x, first, m);
    }
    count = m - first;
    return x + first;
}

}  // namespace micro_flac

#endif  // MICRO_FLAC_ENCODER_DISABLE_LPC
