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

/// @file lpc_analysis.h
/// @brief The encoder's linear prediction (LPC) analysis, in integers
///
/// A windowed autocorrelation fed a chunk at a time, the predictor design
/// (Schur recursion, order selection, quantization) and the residual kernel,
/// on plain int32_t sample arrays. flac_encoder.cpp applies them to subframes.
///
/// Everything is integer arithmetic, so output is identical on every
/// platform, within about 0.02% of a double-precision design. Streams of up to
/// 16 bits run in 32-bit arithmetic, which Xtensa needs to be fast. Deeper
/// streams take a "wide" path in 64-bit arithmetic with a more precise design.
///
/// Nothing here allocates; kernels work in the caller's LPC_WORK_SAMPLES
/// buffer.

#pragma once

#include <cstdint>

namespace micro_flac {

/// Highest predictor order supported (FLACEncoder::MAX_LPC_ORDER)
constexpr uint32_t LPC_MAX_ORDER = 12;

/// Samples one kernel call takes at most
constexpr uint32_t LPC_KERNEL_SAMPLES = 128;

/// Kernel work buffer size: a piece plus the history before it
constexpr uint32_t LPC_WORK_SAMPLES = LPC_MAX_ORDER + LPC_KERNEL_SAMPLES;

/// Deepest subframe the 32-bit path takes: a 16-bit stream's side. Deeper
/// would leave the residual dot product too little coefficient precision.
constexpr uint32_t LPC_NARROW_MAX_SUBFRAME_BPS = 17;

/// Deepest subframe LPC supports: a 24-bit stream's side, on the wide path
constexpr uint32_t LPC_MAX_SUBFRAME_BPS = 25;

/// Narrow-path residual magnitude limit: far past what a useful predictor
/// leaves, and low enough that a kernel piece's magnitudes sum in 32 bits
constexpr int32_t LPC_RESIDUAL_LIMIT = int32_t{1} << 23;

/// Wide-path residual magnitude limit, inside the 32 bits RFC 9639 SS9.2.7
/// requires. Its kernel pieces sum in 64 bits.
constexpr int64_t LPC_WIDE_RESIDUAL_LIMIT = int64_t{1} << 30;

/// A running autocorrelation of a Tukey(0.5)-windowed signal at lags
/// 0..`lags`, fed the whole signal in order through
/// lpc_autocorrelation_update()
struct LpcAutocorrelation {
    int64_t sum[LPC_MAX_ORDER + 1];
    int32_t history[LPC_MAX_ORDER];  // The last LPC_MAX_ORDER windowed samples, oldest first
    uint32_t lags;
    uint32_t n;            // Samples in the signal
    uint32_t position;     // Samples fed so far
    uint32_t taper;        // Last index of the rising taper; 0 for no window
    uint32_t step;         // Window phase per taper sample, Q24
    uint32_t scale_shift;  // Right shift bringing windowed samples into the path's range
    bool wide;             // Whether this signal takes the wide path: bps > 17, or any
                           // subframe of a stream deeper than 16 bits
};

/// Begin the autocorrelation of an n-sample signal of subframe depth bps, on
/// the narrow path up to 17 bits and the wide one above
void lpc_autocorrelation_begin(LpcAutocorrelation& ac, uint32_t n, uint32_t lags, uint32_t bps);

/// lpc_autocorrelation_begin() on the wide path at any depth, for every
/// subframe of a stream deeper than 16 bits: a 17-bit stream's 17-bit
/// subframes need the wide design's precision too.
void lpc_autocorrelation_begin_wide(LpcAutocorrelation& ac, uint32_t n, uint32_t lags,
                                    uint32_t bps);

/// Feed the next m samples. `work` holds LPC_WORK_SAMPLES.
void lpc_autocorrelation_update(LpcAutocorrelation& ac, const int32_t* samples, uint32_t m,
                                int32_t* work);

/// A quantized predictor, as an LPC subframe carries it: residual
/// x[i] - ((sum over j of coefficients[j] * x[i - 1 - j]) >> shift).
struct LpcPredictor {
    uint32_t order;                       // 1..LPC_MAX_ORDER
    uint32_t precision;                   // Coefficient bits, 5..15
    uint32_t shift;                       // 0..15
    int32_t coefficients[LPC_MAX_ORDER];  // Zero past `order`
    bool wide;                            // Residuals need the wide path's 64-bit dot product
};

/// Design the predictor for a finished autocorrelation at subframe depth bps,
/// aiming for `precision` coefficient bits (lowered on the narrow path to
/// keep residuals in 32 bits). Returns false when no usable predictor exists.
bool lpc_design(const LpcAutocorrelation& ac, uint32_t bps, uint32_t precision, LpcPredictor& out);

/// State carried through one walk over a signal's LPC residuals
struct LpcResidualState {
    int32_t history[LPC_MAX_ORDER];  // The last LPC_MAX_ORDER samples, oldest first
    uint32_t position;               // Samples fed so far
    bool in_range;                   // Every residual magnitude so far below the path's limit
};

/// Start a walk over a signal's residuals
inline void lpc_residual_begin(LpcResidualState& state) {
    for (int32_t& h : state.history) {
        h = 0;
    }
    state.position = 0;
    state.in_range = true;
}

/// Feed the next m (<= LPC_KERNEL_SAMPLES) samples and return their residuals'
/// magnitude sum (warmup samples have none). Clears state.in_range, making
/// the sum meaningless, if a residual reaches the path's limit.
uint64_t lpc_residual_sum(const LpcPredictor& predictor, LpcResidualState& state,
                          const int32_t* samples, uint32_t m, int32_t* work);

/// Feed the next m (<= LPC_KERNEL_SAMPLES) samples and compute their residuals
/// in `work`, returning where they start and their count. Exact only for a
/// signal lpc_residual_sum() found in range.
const int32_t* lpc_residuals(const LpcPredictor& predictor, LpcResidualState& state,
                             const int32_t* samples, uint32_t m, int32_t* work, uint32_t& count);

}  // namespace micro_flac
