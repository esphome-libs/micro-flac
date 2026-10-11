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

#include "subframe_coding.h"

#include "compiler.h"
#include "flac_format.h"
#include "pcm_packing.h"

#include <cstddef>
#include <cstdint>

namespace micro_flac {

namespace {

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

}  // namespace

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

FLAC_COLD_SECTION FLAC_NOINLINE int32_t* PackedSignal::unpack32(uint32_t start, uint32_t m) const {
    const size_t frame_bytes = static_cast<size_t>(4) * this->num_channels;
    const uint8_t* src = this->input + (start * frame_bytes);
    const uint32_t shift = this->wasted;
    if (this->num_channels != 2) {
        unpack_channel32(this->buf, src, m, this->channel, this->num_channels, this->bps - shift);
    } else if (this->sig == SignalId::MID) {
        unpack_mid32(this->buf, src, m, this->bps, shift);
    } else if (this->sig == SignalId::SIDE) {
        unpack_side32(this->buf, src, m, this->bps, shift);
    } else {
        const uint32_t ch = (this->sig == SignalId::RIGHT) ? 1 : 0;
        unpack_channel32(this->buf, src, m, ch, 2, this->bps - shift);
    }
    return this->buf;
}

FLAC_NOINLINE void write_warmup(BitWriterLocal& bw, const PackedSignal& signal, uint32_t order,
                                uint8_t bps8) {
    for (uint32_t i = 0; i < order; i++) {
        write_sample(bw, signal.warmup_sample(i), bps8);
    }
}

}  // namespace micro_flac
