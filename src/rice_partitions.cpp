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

#include "rice_partitions.h"

#include "bit_writer.h"
#include "compiler.h"
#include "flac_format.h"
#include "subframe_coding.h"

#include <cstddef>
#include <cstdint>

namespace micro_flac {

#ifndef MICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS

namespace {

// One sink type for the walks below, so they share their signal's
// residual_chunks() instantiations instead of each adding five
struct ResidualSink {
    void (*fn)(void* context, const int32_t* r, uint32_t m);
    void* context;

    void operator()(const int32_t* r, uint32_t m) const {
        this->fn(this->context, r, m);
    }
};

// add_leaf_residuals() in 64 bits, for subframes deeper than 25 bits, whose
// residuals may reach 2^31 - 1
FLAC_COLD_SECTION FLAC_NOINLINE void add_leaf_residuals64(LeafSums& sums, const int32_t* r,
                                                          uint32_t m) {
    while (m != 0) {
        const uint32_t take = leaf_step(sums, m);
        uint64_t sum = 0;
        for (uint32_t j = 0; j < take; j++) {
            sum += abs_u32(r[j]);
        }
        sums.sum[sums.index] += sum;
        r += take;
        m -= take;
        sums.pos += take;
    }
}

// Add the next m residuals to their leaves' sums, in uint32_t partials of at
// most sum_chunk_samples(bps)
FLAC_NOINLINE void add_leaf_residuals(LeafSums& sums, const int32_t* r, uint32_t m, uint32_t bps) {
    if (FLAC_UNLIKELY(bps > MAX_SCAN32_BITS_PER_SAMPLE + 1)) {
        add_leaf_residuals64(sums, r, m);
        return;
    }
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

}  // namespace

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

// Out of line so the leaf sums take stack only here
FLAC_NOINLINE uint64_t partition_fixed(const PackedSignal& signal, uint32_t bps,
                                       const ChannelAnalysis& analysis, uint32_t max_order,
                                       RicePartitions& rice) {
    const uint32_t n = signal.n;
    const uint32_t order = analysis.order;
    const uint32_t p = partition_order_limit(n, order, max_order);
    if (p == 0 || FLAC_UNLIKELY(analysis.verbatim)) {
        return unpartitioned_fixed(analysis, rice);  // VERBATIM: left to Pass B's bound
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

}  // namespace micro_flac
