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

#include "pcm_packing.h"

#include "compiler.h"
#include "wrapping_arithmetic.h"

#include <cstddef>
#include <cstring>
#include <type_traits>

namespace micro_flac {

// ============================================================================
// Packing
// ============================================================================

FLAC_OPTIMIZE_O3
static void write_samples_nch(uint8_t* output_buffer, const int32_t* block_samples,
                              uint32_t block_size, uint32_t bytes_per_sample, uint32_t shift_amount,
                              uint32_t num_channels) {
    uint32_t output_index = 0;

    for (uint32_t i = 0; i < block_size; i++) {
        for (uint32_t j = 0; j < num_channels; j++) {
            int32_t sample = block_samples[(j * block_size) + i];
            sample = wshl32(sample, shift_amount);

            for (uint32_t byte = 0; byte < bytes_per_sample; byte++) {
                output_buffer[output_index++] =
                    static_cast<uint8_t>(wshr32(sample, byte * 8) & 0xFF);
            }
        }
    }
}

FLAC_OPTIMIZE_O3
static void write_samples_16bit_1ch(uint8_t* output_buffer, const int32_t* block_samples,
                                    uint32_t block_size) {
    // 16-bit mono fast path with pointer arithmetic and loop unrolling
    int16_t* output_samples = reinterpret_cast<int16_t*>(output_buffer);
    const int32_t* samples = block_samples;

    uint32_t i = 0;
    const uint32_t unroll_limit = block_size & ~3U;  // Round down to multiple of 4

    for (; i < unroll_limit; i += 4) {
        output_samples[i] = static_cast<int16_t>(samples[i]);
        output_samples[i + 1] = static_cast<int16_t>(samples[i + 1]);
        output_samples[i + 2] = static_cast<int16_t>(samples[i + 2]);
        output_samples[i + 3] = static_cast<int16_t>(samples[i + 3]);
    }

    for (; i < block_size; ++i) {
        output_samples[i] = static_cast<int16_t>(samples[i]);
    }
}

FLAC_OPTIMIZE_O3
static void write_samples_16bit_2ch(uint8_t* output_buffer, const int32_t* block_samples,
                                    uint32_t block_size) {
    // 16-bit stereo fast path with pointer arithmetic and 4-sample unrolling
    int16_t* output_samples = reinterpret_cast<int16_t*>(output_buffer);
    const int32_t* left = block_samples;
    const int32_t* right = block_samples + block_size;

    uint32_t i = 0;
    const uint32_t unroll_limit = block_size & ~3U;  // Round down to multiple of 4

    for (; i < unroll_limit; i += 4) {
        output_samples[0] = static_cast<int16_t>(left[i]);
        output_samples[1] = static_cast<int16_t>(right[i]);
        output_samples[2] = static_cast<int16_t>(left[i + 1]);
        output_samples[3] = static_cast<int16_t>(right[i + 1]);
        output_samples[4] = static_cast<int16_t>(left[i + 2]);
        output_samples[5] = static_cast<int16_t>(right[i + 2]);
        output_samples[6] = static_cast<int16_t>(left[i + 3]);
        output_samples[7] = static_cast<int16_t>(right[i + 3]);
        output_samples += 8;
    }

    for (; i < block_size; ++i) {
        output_samples[0] = static_cast<int16_t>(left[i]);
        output_samples[1] = static_cast<int16_t>(right[i]);
        output_samples += 2;
    }
}

FLAC_OPTIMIZE_O3
static void write_samples_24bit_2ch_aligned(uint8_t* output_buffer, const int32_t* block_samples,
                                            uint32_t block_size) {
    // 24-bit stereo fast path for 4-byte-aligned buffers. Packs 2 stereo
    // pairs (12 bytes) into 3 uint32_t stores per iteration instead of 12
    // byte stores. Caller has verified output_buffer alignment.
    //
    // Byte layout of 2 stereo pairs in memory (little-endian):
    //   L0[0] L0[1] L0[2] R0[0]   R0[1] R0[2] L1[0] L1[1]   L1[2] R1[0] R1[1] R1[2]
    //   \------ word0 ------/     \------ word1 ------/     \------ word2 ------/
    uint32_t* out32 = reinterpret_cast<uint32_t*>(output_buffer);
    const int32_t* left = block_samples;
    const int32_t* right = block_samples + block_size;

    uint32_t i = 0;
    const uint32_t unroll_limit = block_size & ~1U;

    for (; i < unroll_limit; i += 2) {
        const uint32_t l0 = static_cast<uint32_t>(left[i]);
        const uint32_t r0 = static_cast<uint32_t>(right[i]);
        const uint32_t l1 = static_cast<uint32_t>(left[i + 1]);
        const uint32_t r1 = static_cast<uint32_t>(right[i + 1]);

        out32[0] = (l0 & 0xFFFFFFU) | (r0 << 24);       // NOLINT(readability-magic-numbers)
        out32[1] = ((r0 >> 8) & 0xFFFFU) | (l1 << 16);  // NOLINT(readability-magic-numbers)
        out32[2] = ((l1 >> 16) & 0xFFU) | (r1 << 8);    // NOLINT(readability-magic-numbers)
        out32 += 3;
    }

    // Odd-count tail: one stereo sample (6 bytes) remains. Fall back to byte
    // stores for the final pair to keep the 2-sample fast path simple.
    if (i < block_size) {
        uint8_t* tail = reinterpret_cast<uint8_t*>(out32);
        const int32_t sample_l = left[i];
        const int32_t sample_r = right[i];
        tail[0] = static_cast<uint8_t>(sample_l & 0xFF);
        tail[1] = static_cast<uint8_t>((sample_l >> 8) & 0xFF);
        tail[2] = static_cast<uint8_t>((sample_l >> 16) & 0xFF);
        tail[3] = static_cast<uint8_t>(sample_r & 0xFF);
        tail[4] = static_cast<uint8_t>((sample_r >> 8) & 0xFF);
        tail[5] = static_cast<uint8_t>((sample_r >> 16) & 0xFF);
    }
}

FLAC_OPTIMIZE_O3
static void write_samples_24bit_2ch(uint8_t* output_buffer, const int32_t* block_samples,
                                    uint32_t block_size) {
    // 24-bit stereo fast path with 2-sample unrolling (due to larger sample size)
    uint32_t output_index = 0;
    uint32_t i = 0;
    const uint32_t unroll_limit = block_size & ~1U;  // Round down to multiple of 2

    for (; i < unroll_limit; i += 2) {
        // Sample 0 - Left and Right channels
        int32_t sample_0_l = block_samples[i];
        int32_t sample_0_r = block_samples[block_size + i];
        // Sample 1 - Left and Right channels
        int32_t sample_1_l = block_samples[i + 1];
        int32_t sample_1_r = block_samples[block_size + i + 1];

        // Direct 24-bit writes (little-endian)
        output_buffer[output_index++] = static_cast<uint8_t>(sample_0_l & 0xFF);
        output_buffer[output_index++] = static_cast<uint8_t>((sample_0_l >> 8) & 0xFF);
        output_buffer[output_index++] = static_cast<uint8_t>((sample_0_l >> 16) & 0xFF);
        output_buffer[output_index++] = static_cast<uint8_t>(sample_0_r & 0xFF);
        output_buffer[output_index++] = static_cast<uint8_t>((sample_0_r >> 8) & 0xFF);
        output_buffer[output_index++] = static_cast<uint8_t>((sample_0_r >> 16) & 0xFF);

        output_buffer[output_index++] = static_cast<uint8_t>(sample_1_l & 0xFF);
        output_buffer[output_index++] = static_cast<uint8_t>((sample_1_l >> 8) & 0xFF);
        output_buffer[output_index++] = static_cast<uint8_t>((sample_1_l >> 16) & 0xFF);
        output_buffer[output_index++] = static_cast<uint8_t>(sample_1_r & 0xFF);
        output_buffer[output_index++] = static_cast<uint8_t>((sample_1_r >> 8) & 0xFF);
        output_buffer[output_index++] = static_cast<uint8_t>((sample_1_r >> 16) & 0xFF);
    }

    for (; i < block_size; ++i) {
        int32_t sample_l = block_samples[i];
        int32_t sample_r = block_samples[block_size + i];

        output_buffer[output_index++] = static_cast<uint8_t>(sample_l & 0xFF);
        output_buffer[output_index++] = static_cast<uint8_t>((sample_l >> 8) & 0xFF);
        output_buffer[output_index++] = static_cast<uint8_t>((sample_l >> 16) & 0xFF);
        output_buffer[output_index++] = static_cast<uint8_t>(sample_r & 0xFF);
        output_buffer[output_index++] = static_cast<uint8_t>((sample_r >> 8) & 0xFF);
        output_buffer[output_index++] = static_cast<uint8_t>((sample_r >> 16) & 0xFF);
    }
}

FLAC_OPTIMIZE_O3
static void write_samples_32bit_1ch(uint8_t* output_buffer, const int32_t* block_samples,
                                    uint32_t block_size, uint32_t shift_amount) {
    // 32-bit mono fast path with pointer arithmetic and loop unrolling
    int32_t* output_samples = reinterpret_cast<int32_t*>(output_buffer);
    const int32_t* samples = block_samples;

    uint32_t i = 0;
    const uint32_t unroll_limit = block_size & ~3U;  // Round down to multiple of 4

    for (; i < unroll_limit; i += 4) {
        output_samples[i] = wshl32(samples[i], shift_amount);
        output_samples[i + 1] = wshl32(samples[i + 1], shift_amount);
        output_samples[i + 2] = wshl32(samples[i + 2], shift_amount);
        output_samples[i + 3] = wshl32(samples[i + 3], shift_amount);
    }

    for (; i < block_size; ++i) {
        output_samples[i] = wshl32(samples[i], shift_amount);
    }
}

FLAC_OPTIMIZE_O3
static void write_samples_32bit_2ch(uint8_t* output_buffer, const int32_t* block_samples,
                                    uint32_t block_size, uint32_t shift_amount) {
    // 32-bit stereo fast path with pointer arithmetic and 4-sample unrolling
    int32_t* output_samples = reinterpret_cast<int32_t*>(output_buffer);
    const int32_t* left = block_samples;
    const int32_t* right = block_samples + block_size;

    uint32_t i = 0;
    const uint32_t unroll_limit = block_size & ~3U;  // Round down to multiple of 4

    for (; i < unroll_limit; i += 4) {
        output_samples[0] = wshl32(left[i], shift_amount);
        output_samples[1] = wshl32(right[i], shift_amount);
        output_samples[2] = wshl32(left[i + 1], shift_amount);
        output_samples[3] = wshl32(right[i + 1], shift_amount);
        output_samples[4] = wshl32(left[i + 2], shift_amount);
        output_samples[5] = wshl32(right[i + 2], shift_amount);
        output_samples[6] = wshl32(left[i + 3], shift_amount);
        output_samples[7] = wshl32(right[i + 3], shift_amount);
        output_samples += 8;
    }

    for (; i < block_size; ++i) {
        output_samples[0] = wshl32(left[i], shift_amount);
        output_samples[1] = wshl32(right[i], shift_amount);
        output_samples += 2;
    }
}

FLAC_OPTIMIZE_O3
static void write_samples_32bit_nch(uint8_t* output_buffer, const int32_t* block_samples,
                                    uint32_t block_size, uint32_t shift_amount,
                                    uint32_t num_channels) {
    int32_t* output_samples = reinterpret_cast<int32_t*>(output_buffer);
    uint32_t output_index = 0;

    for (uint32_t i = 0; i < block_size; ++i) {
        for (uint32_t ch = 0; ch < num_channels; ++ch) {
            output_samples[output_index++] =
                wshl32(block_samples[ch * block_size + i], shift_amount);
        }
    }
}

void write_samples(uint8_t* output_buffer, const int32_t* block_samples, uint32_t block_size,
                   uint32_t bits_per_sample, uint32_t num_channels, bool output_32bit) {
    // Check output buffer alignment for fast paths that use reinterpret_cast.
    const bool aligned_4 = (reinterpret_cast<uintptr_t>(output_buffer) % 4) == 0;
    const bool aligned_2 = (reinterpret_cast<uintptr_t>(output_buffer) % 2) == 0;

    if (output_32bit) {
        // 32-bit output mode: all samples output as 4 bytes, left-justified (MSB-aligned)
        uint32_t shift_amount = 32 - bits_per_sample;

        if (aligned_4 && num_channels == 1) {
            write_samples_32bit_1ch(output_buffer, block_samples, block_size, shift_amount);
        } else if (aligned_4 && num_channels == 2) {
            write_samples_32bit_2ch(output_buffer, block_samples, block_size, shift_amount);
        } else if (aligned_4) {
            write_samples_32bit_nch(output_buffer, block_samples, block_size, shift_amount,
                                    num_channels);
        } else {
            write_samples_nch(output_buffer, block_samples, block_size, 4, shift_amount,
                              num_channels);
        }
    } else {
        // Native output mode: pack to nearest byte boundary
        uint32_t shift_amount = 0;
        if (bits_per_sample % 8 != 0) {
            shift_amount = 8 - (bits_per_sample % 8);
        }

        if (aligned_2 && bits_per_sample == 16 && num_channels == 1) {
            write_samples_16bit_1ch(output_buffer, block_samples, block_size);
        } else if (aligned_2 && bits_per_sample == 16 && num_channels == 2) {
            write_samples_16bit_2ch(output_buffer, block_samples, block_size);
        } else if (aligned_4 && bits_per_sample == 24 && num_channels == 2) {
            write_samples_24bit_2ch_aligned(output_buffer, block_samples, block_size);
        } else if (bits_per_sample == 24 && num_channels == 2) {
            write_samples_24bit_2ch(output_buffer, block_samples, block_size);
        } else {
            uint32_t bytes_per_sample = (bits_per_sample + 7) / 8;
            write_samples_nch(output_buffer, block_samples, block_size, bytes_per_sample,
                              shift_amount, num_channels);
        }
    }
}

// ============================================================================
// Unpacking
// ============================================================================

namespace {

// Call body(shift) with shift = 32 - bps, the arithmetic shift that turns a
// sample at the top of a word into its value. Full-width depths get a
// compile-time constant, which Xtensa shifts by in one instruction.
template <uint32_t BYTES, typename Body>
FLAC_ALWAYS_INLINE void with_sample_shift(uint32_t bps, Body body) {
    if (bps == 8 * BYTES) {
        body(std::integral_constant<uint32_t, 32 - (8 * BYTES)>{});
    } else {
        body(32 - bps);
    }
}

// One packed sample, placed at the top of a word and shifted down. 2-byte
// samples are one aligned 16-bit load; 1- and 3-byte ones are byte loads.
template <uint32_t BYTES, typename Shift>
FLAC_ALWAYS_INLINE int32_t read_packed_sample(const uint8_t* p, Shift shift) {
    uint32_t top = 0;
    if (BYTES == 2) {
        top = static_cast<uint32_t>(
                  static_cast<uint16_t>(*reinterpret_cast<const aliased_int16_t*>(p)))
              << 16;
    } else {
        top = static_cast<uint32_t>(p[BYTES - 1]) << 24;
        if (BYTES == 3) {
            top |= (static_cast<uint32_t>(p[0]) << 8) | (static_cast<uint32_t>(p[1]) << 16);
        }
    }
    return static_cast<int32_t>(top) >> shift;
}

#ifdef FLAC_FAST_UNALIGNED_LOADS
// read_packed_sample<3>() as one unaligned 32-bit load, on hosts where that
// is cheap. The fourth byte, the next one in the input, must be readable, so
// callers leave each run's last frame to the byte loads.
template <typename Shift>
FLAC_ALWAYS_INLINE int32_t read_packed_sample_24_load(const uint8_t* p, Shift shift) {
    uint32_t word = 0;
    std::memcpy(&word, p, sizeof(word));
    return static_cast<int32_t>(word << 8) >> shift;
}
#endif

// Deinterleave channel `ch` of `n` frames into `dst`. Aligned 24-bit mono and
// stereo input is read three words per 12 bytes, as read_stereo() diagrams,
// at about half the instructions of byte loads.
template <uint32_t BYTES>
void fill_channel(int32_t* dst, const uint8_t* src, uint32_t n, uint32_t ch, uint32_t num_channels,
                  uint32_t bps) {
    const size_t frame_bytes = static_cast<size_t>(BYTES) * num_channels;
    const uint8_t* p = src + (static_cast<size_t>(BYTES) * ch);
    with_sample_shift<BYTES>(bps, [&](auto shift) {
        uint32_t i = 0;
        if (BYTES == 3 && num_channels <= 2 && (reinterpret_cast<uintptr_t>(src) & 3U) == 0) {
            // NOLINTBEGIN(readability-magic-numbers) -- byte lanes per read_stereo()'s layout
            const aliased_uint32_t* w = reinterpret_cast<const aliased_uint32_t*>(src);
            if (num_channels == 1) {
                const uint32_t quad_end = n & ~3U;
                for (; i < quad_end; i += 4) {
                    const uint32_t w0 = w[0];
                    const uint32_t w1 = w[1];
                    const uint32_t w2 = w[2];
                    w += 3;
                    dst[i] = static_cast<int32_t>(w0 << 8) >> shift;
                    dst[i + 1] = static_cast<int32_t>((w1 << 16) | ((w0 >> 16) & 0xFF00U)) >> shift;
                    dst[i + 2] =
                        static_cast<int32_t>((w2 << 24) | ((w1 >> 8) & 0xFFFF00U)) >> shift;
                    dst[i + 3] = static_cast<int32_t>(w2 & 0xFFFFFF00U) >> shift;
                }
            } else if (ch == 0) {
                const uint32_t pair_end = n & ~1U;
                for (; i < pair_end; i += 2) {
                    const uint32_t w0 = w[0];
                    const uint32_t w1 = w[1];
                    const uint32_t w2 = w[2];
                    w += 3;
                    dst[i] = static_cast<int32_t>(w0 << 8) >> shift;
                    dst[i + 1] =
                        static_cast<int32_t>((w2 << 24) | ((w1 >> 8) & 0xFFFF00U)) >> shift;
                }
            } else {
                const uint32_t pair_end = n & ~1U;
                for (; i < pair_end; i += 2) {
                    const uint32_t w0 = w[0];
                    const uint32_t w1 = w[1];
                    const uint32_t w2 = w[2];
                    w += 3;
                    dst[i] = static_cast<int32_t>((w1 << 16) | ((w0 >> 16) & 0xFF00U)) >> shift;
                    dst[i + 1] = static_cast<int32_t>(w2 & 0xFFFFFF00U) >> shift;
                }
            }
            // NOLINTEND(readability-magic-numbers)
        }
#ifdef FLAC_FAST_UNALIGNED_LOADS
        if (BYTES == 3) {
            for (; i + 1 < n; i++) {  // Frame i + 1 follows the sample's fourth byte
                dst[i] =
                    read_packed_sample_24_load(p + (static_cast<size_t>(i) * frame_bytes), shift);
            }
        }
#endif
        for (; i < n; i++) {
            dst[i] = read_packed_sample<BYTES>(p + (static_cast<size_t>(i) * frame_bytes), shift);
        }
    });
}

// Read both channels of `n` stereo frames in one pass, handing each frame's
// samples to store(i, left, right). Aligned 24-bit input is read a word at a
// time, the reverse of write_samples_24bit_2ch_aligned(): two frames are
// exactly three words,
//
//   L0[0] L0[1] L0[2] R0[0]   R0[1] R0[2] L1[0] L1[1]   L1[2] R1[0] R1[1] R1[2]
//   \------ word0 ------/     \------ word1 ------/     \------ word2 ------/
//
// and each sample is reassembled at the top of a word from them. An odd
// final frame takes byte loads.
template <uint32_t BYTES, typename Store>
FLAC_ALWAYS_INLINE void read_stereo(const uint8_t* src, uint32_t n, uint32_t bps, Store store) {
    with_sample_shift<BYTES>(bps, [&](auto shift) {
        uint32_t i = 0;
        if (BYTES == 3 && (reinterpret_cast<uintptr_t>(src) & 3U) == 0) {
            // NOLINTBEGIN(readability-magic-numbers) -- byte lanes per the layout above
            const aliased_uint32_t* w = reinterpret_cast<const aliased_uint32_t*>(src);
            const uint32_t pair_end = n & ~1U;
            for (; i < pair_end; i += 2) {
                const uint32_t w0 = w[0];
                const uint32_t w1 = w[1];
                const uint32_t w2 = w[2];
                w += 3;
                store(i, static_cast<int32_t>(w0 << 8) >> shift,
                      static_cast<int32_t>((w1 << 16) | ((w0 >> 16) & 0xFF00U)) >> shift);
                store(i + 1, static_cast<int32_t>((w2 << 24) | ((w1 >> 8) & 0xFFFF00U)) >> shift,
                      static_cast<int32_t>(w2 & 0xFFFFFF00U) >> shift);
            }
            // NOLINTEND(readability-magic-numbers)
        }
#ifdef FLAC_FAST_UNALIGNED_LOADS
        if (BYTES == 3) {
            for (; i + 1 < n; i++) {  // Frame i + 1 follows both samples
                const uint8_t* p = src + (static_cast<size_t>(i) * 2 * BYTES);
                store(i, read_packed_sample_24_load(p, shift),
                      read_packed_sample_24_load(p + BYTES, shift));
            }
        }
#endif
        for (; i < n; i++) {
            const uint8_t* p = src + (static_cast<size_t>(i) * 2 * BYTES);
            store(i, read_packed_sample<BYTES>(p, shift),
                  read_packed_sample<BYTES>(p + BYTES, shift));
        }
    });
}

template <uint32_t BYTES>
void fill_stereo(int32_t* left, int32_t* right, const uint8_t* src, uint32_t n, uint32_t bps) {
    read_stereo<BYTES>(src, n, bps, [=](uint32_t i, int32_t l, int32_t r) {
        left[i] = l;
        right[i] = r;
    });
}

// Derive mid (SIDE false) or side straight from packed stereo, without
// storing left and right first. No wasted bits gets a constant shift.
template <uint32_t BYTES, bool SIDE>
void fill_mid_side(int32_t* dst, const uint8_t* src, uint32_t n, uint32_t bps, uint32_t wasted) {
    const auto derive = [&](auto down) {
        read_stereo<BYTES>(src, n, bps, [=](uint32_t i, int32_t l, int32_t r) {
            dst[i] = SIDE ? ((l - r) >> down) : ((l + r) >> (down + 1));
        });
    };
    if (wasted == 0) {
        derive(std::integral_constant<uint32_t, 0>{});
    } else {
        derive(wasted);
    }
}

template <bool SIDE>
void dispatch_mid_side(int32_t* dst, const uint8_t* src, uint32_t n, uint32_t bps, uint32_t bytes,
                       uint32_t wasted) {
    switch (bytes) {
        case 1:
            fill_mid_side<1, SIDE>(dst, src, n, bps, wasted);
            break;
        case 2:
            fill_mid_side<2, SIDE>(dst, src, n, bps, wasted);
            break;
        default:  // 3
            fill_mid_side<3, SIDE>(dst, src, n, bps, wasted);
            break;
    }
}

}  // namespace

void unpack_channel(int32_t* dst, const uint8_t* src, uint32_t n, uint32_t ch,
                    uint32_t num_channels, uint32_t bps, uint32_t bytes) {
    switch (bytes) {
        case 1:
            fill_channel<1>(dst, src, n, ch, num_channels, bps);
            break;
        case 2:
            fill_channel<2>(dst, src, n, ch, num_channels, bps);
            break;
        default:  // 3
            fill_channel<3>(dst, src, n, ch, num_channels, bps);
            break;
    }
}

void unpack_stereo(int32_t* left, int32_t* right, const uint8_t* src, uint32_t n, uint32_t bps,
                   uint32_t bytes) {
    switch (bytes) {
        case 1:
            fill_stereo<1>(left, right, src, n, bps);
            break;
        case 2:
            fill_stereo<2>(left, right, src, n, bps);
            break;
        default:  // 3
            fill_stereo<3>(left, right, src, n, bps);
            break;
    }
}

void unpack_mid(int32_t* dst, const uint8_t* src, uint32_t n, uint32_t bps, uint32_t bytes,
                uint32_t wasted) {
    dispatch_mid_side<false>(dst, src, n, bps, bytes, wasted);
}

void unpack_side(int32_t* dst, const uint8_t* src, uint32_t n, uint32_t bps, uint32_t bytes,
                 uint32_t wasted) {
    dispatch_mid_side<true>(dst, src, n, bps, bytes, wasted);
}

int32_t unpack_sample(const uint8_t* p, uint32_t bps, uint32_t bytes) {
    const uint32_t shift = 32 - bps;
    switch (bytes) {
        case 1:
            return read_packed_sample<1>(p, shift);
        case 2:
            return read_packed_sample<2>(p, shift);
        default:  // 3
            return read_packed_sample<3>(p, shift);
    }
}

}  // namespace micro_flac
