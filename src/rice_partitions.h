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

/// @file rice_partitions.h
/// @brief Rice partitioning (FLACEncoderOptions::max_rice_partition_order)
///
/// A subframe's residuals can be split into 2^p equal partitions, the first
/// short by the predictor order, each with its own Rice parameter (RFC 9639
/// SS9.2.7). It is chosen after the predictor, in Pass B (or when the stereo
/// search prices its LPC candidates): one walk sums the residual magnitudes
/// per partition at the highest order allowed (the leaves), and
/// choose_partitions() prices every order from those sums, merging neighbors
/// bottom-up as libFLAC does. Pass A is unchanged. The size bound holds
/// partition by partition, so estimate + (n - order) / 2 still bounds the
/// subframe.
///
/// A subframe that does not split takes the single-partition walks, so with
/// partitioning off, every other option runs the same code. Only
/// RicePartitions exists in a build without partitioning.

#pragma once

#include "bit_writer.h"
#include "compiler.h"
#include "micro_flac/flac_encoder.h"
#include "subframe_coding.h"

#include <cstdint>

namespace micro_flac {

#ifndef MICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS
constexpr uint32_t MAX_PARTITION_ORDER = FLACEncoder::MAX_RICE_PARTITION_ORDER;
#else
constexpr uint32_t MAX_PARTITION_ORDER = 0;
#endif
constexpr uint32_t MAX_PARTITIONS = 1U << MAX_PARTITION_ORDER;

// A subframe's Rice coding: 2^order partitions and each one's parameter
struct RicePartitions {
    uint8_t order;
    uint8_t parameter_bits;  // 4, or 5 when any parameter exceeds RICE_PARAMETER_MAX_4BIT
    uint8_t k[MAX_PARTITIONS];
};

#ifndef MICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS

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
uint64_t choose_partitions(LeafSums& sums, uint32_t n, uint32_t order, uint32_t p,
                           RicePartitions& out);

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
void emit_partitioned(BitWriterLocal& bw, RiceEmitter& e, const int32_t* r, uint32_t m);

// The partitioning of the FIXED subframe at Pass A's order that Pass B will
// write, and its estimated size. A split is kept only when its bound proves
// it smaller than VERBATIM (as write_partitioned_fixed() needs), so LPC is
// weighed against the FIXED subframe actually written; a dropped split, or a
// subframe too short to split, leaves analysis.estimated_bits.
uint64_t partition_fixed(const PackedSignal& signal, uint32_t bps, const ChannelAnalysis& analysis,
                         uint32_t max_order, RicePartitions& rice);

// Write the FIXED subframe partitioned as `rice`, which partition_fixed()
// kept only when its bound proves it smaller than VERBATIM
void write_partitioned_fixed(BitWriterLocal& bw, const PackedSignal& signal, uint32_t bps,
                             uint8_t order, const RicePartitions& rice, uint64_t estimated_bits);

#endif  // MICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS

}  // namespace micro_flac
