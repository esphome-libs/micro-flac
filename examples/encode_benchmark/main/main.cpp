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

/**
 * FLAC Encode Benchmark for ESP32
 *
 * Measures FLAC encoding performance including:
 * - Per-frame encode timing (min/max/avg/stddev)
 * - Total encode time
 * - Real-Time Factor (RTF) and realtime multiple
 * - Achieved bytes/sample and compression ratio
 *
 * Audio source: the same embedded 16-bit FLAC clip used by the
 * decode_benchmark example (public domain music from Musopen), decoded to
 * PCM once at startup so the encoder runs against real music instead of a
 * synthetic signal.
 */

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "micro_flac/flac_decoder.h"
#include "micro_flac/flac_encoder.h"
#include "micro_flac/pcm_format.h"
#include "test_audio_flac.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace micro_flac;

namespace {

// ============================================================================
// Test audio loading
// ============================================================================

// Sample rate and channel count of the decoded test clip, filled in by
// decode_test_audio() before any encode benchmark runs.
uint32_t g_sample_rate = 0;
uint32_t g_num_channels = 0;

// Decodes the embedded 16-bit FLAC test clip (shared with the
// decode_benchmark example, via test_audio_flac.h) into an interleaved
// int16 PCM buffer in PSRAM, so the encoder benchmark below exercises real
// music instead of a synthetic signal. Returns nullptr on failure.
int16_t* decode_test_audio(uint32_t& out_sample_rate, uint32_t& out_channels,
                           uint32_t& out_samples_per_channel) {
    FLACDecoder decoder;

    const uint8_t* input_ptr = test_audio_flac_data;
    size_t bytes_remaining = test_audio_flac_data_len;

    // Scratch buffer sized for one decode() call's worth of PCM; the decoded
    // audio is copied out of it into `pcm` below.
    uint8_t* frame_buffer = nullptr;
    size_t frame_buffer_size = 0;
    int16_t* pcm = nullptr;
    uint32_t samples_written = 0;  // per channel

    while (bytes_remaining > 0) {
        size_t consumed = 0;
        size_t samples_decoded = 0;

        const FLACDecoderResult result = decoder.decode(
            input_ptr, bytes_remaining, frame_buffer, frame_buffer_size, consumed, samples_decoded);
        input_ptr += consumed;
        bytes_remaining -= consumed;

        if (result == FLAC_DECODER_HEADER_READY) {
            const FLACStreamInfo& stream_info = decoder.get_stream_info();
            out_sample_rate = stream_info.sample_rate();
            out_channels = stream_info.num_channels();
            out_samples_per_channel =
                static_cast<uint32_t>(stream_info.total_samples_per_channel());
            // `pcm` is sized from STREAMINFO and holds int16_t, so both must be known
            if (out_samples_per_channel == 0 || stream_info.bits_per_sample() != 16) {
                printf("ERROR: test audio must be 16-bit with a known sample count\n");
                return nullptr;
            }

            frame_buffer_size = static_cast<size_t>(decoder.get_output_buffer_size_samples()) *
                                stream_info.bytes_per_sample();
            frame_buffer = static_cast<uint8_t*>(malloc(frame_buffer_size));
            if (!frame_buffer) {
                printf("ERROR: failed to allocate %zu byte decode scratch buffer\n",
                       frame_buffer_size);
                return nullptr;
            }

            const size_t pcm_bytes =
                static_cast<size_t>(out_samples_per_channel) * out_channels * sizeof(int16_t);
            pcm = static_cast<int16_t*>(
                heap_caps_malloc(pcm_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!pcm) {
                pcm = static_cast<int16_t*>(heap_caps_malloc(pcm_bytes, MALLOC_CAP_8BIT));
            }
            if (!pcm) {
                printf("ERROR: failed to allocate %zu byte PCM buffer\n", pcm_bytes);
                free(frame_buffer);
                return nullptr;
            }
        } else if (result == FLAC_DECODER_SUCCESS) {
            // samples_decoded is interleaved across all channels. The decoder
            // does not stop at STREAMINFO's count, so refuse audio past it.
            if (samples_written + (samples_decoded / out_channels) > out_samples_per_channel) {
                printf("ERROR: test audio is longer than its STREAMINFO sample count\n");
                free(frame_buffer);
                heap_caps_free(pcm);
                return nullptr;
            }
            memcpy(pcm + static_cast<size_t>(samples_written) * out_channels, frame_buffer,
                   samples_decoded * sizeof(int16_t));
            samples_written += static_cast<uint32_t>(samples_decoded / out_channels);
        } else if (result == FLAC_DECODER_END_OF_STREAM) {
            break;
        } else if (result == FLAC_DECODER_NEED_MORE_DATA) {
            // Continue feeding data.
        } else {
            printf("ERROR: decode of embedded test audio failed with code %d\n",
                   static_cast<int>(result));
            free(frame_buffer);
            heap_caps_free(pcm);
            return nullptr;
        }
    }

    free(frame_buffer);
    out_samples_per_channel = samples_written;
    return pcm;
}

// ============================================================================
// Benchmark
// ============================================================================

// How many times each test case re-encodes the full decoded clip, to get
// enough frames for a stable per-frame timing distribution and to amortize
// any one-time warmup cost (cache fills, etc.) over the total.
constexpr uint32_t NUM_PASSES = 8;

// Channel assignment codes (RFC 9639 SS9.1.3), the upper nibble of frame
// header byte 3. Duplicated from src/flac_format.h, which the ESP-IDF
// component keeps private.
constexpr uint8_t CHANNEL_ASSIGNMENT_INDEPENDENT = 1;
constexpr uint8_t CHANNEL_ASSIGNMENT_LEFT_SIDE = 8;
constexpr uint8_t CHANNEL_ASSIGNMENT_RIGHT_SIDE = 9;
constexpr uint8_t CHANNEL_ASSIGNMENT_MID_SIDE = 10;

struct EncodeBenchmarkStats {
    const char* label = "";
    uint32_t bits_per_sample = 16;
    uint32_t num_channels = 2;
    uint32_t block_size = 0;
    uint32_t frame_count = 0;
    int64_t total_time_us = 0;
    int64_t min_time_us = INT64_MAX;
    int64_t max_time_us = 0;
    double sum_squared = 0.0;  // For standard deviation calculation

    uint64_t samples_encoded = 0;  // Per channel, summed across all passes
    uint64_t output_bytes = 0;     // Total FLAC bytes produced (incl. stream header once)

    uint32_t assignment_independent = 0;
    uint32_t assignment_left_side = 0;
    uint32_t assignment_right_side = 0;
    uint32_t assignment_mid_side = 0;
};

// Widens `count` samples of the 16-bit clip to packed 24-bit PCM (3 bytes
// each, little-endian): every sample shifted up 8 bits, with pseudo-random
// low bits so the extra byte carries noise the way a real 24-bit recording's
// does instead of compressing away. Fixed xorshift32 seed, so every run
// widens the clip identically.
void widen_to_24bit(uint8_t* dst, const int16_t* src, size_t count) {
    uint32_t rng = 0x2468ACE1U;
    for (size_t i = 0; i < count; i++) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        const uint32_t v =
            (static_cast<uint32_t>(static_cast<int32_t>(src[i])) << 8) | (rng & 0xFFU);
        dst[(3 * i) + 0] = static_cast<uint8_t>(v);
        dst[(3 * i) + 1] = static_cast<uint8_t>(v >> 8);
        dst[(3 * i) + 2] = static_cast<uint8_t>(v >> 16);
    }
}

// Encodes NUM_PASSES passes over the clip as one stream, timing every
// encode() call. Only whole blocks are encoded, so every frame is block_size
// samples. `input` is the clip as packed PCM at `bits_per_sample` (16 or 24).
EncodeBenchmarkStats run_encode_benchmark(const uint8_t* input, uint32_t samples_per_channel,
                                          const char* label, uint32_t bits_per_sample,
                                          uint32_t num_channels, uint32_t block_size,
                                          uint32_t lpc_order, bool lpc_stereo_search,
                                          uint32_t partition_order) {
    EncodeBenchmarkStats stats;
    stats.label = label;
    stats.bits_per_sample = bits_per_sample;
    stats.num_channels = num_channels;
    stats.block_size = block_size;

    const PcmFormat format(g_sample_rate, num_channels, bits_per_sample);

    FLACEncoderOptions options;
    options.block_size = block_size;
    options.lpc = (lpc_order != 0);
    if (lpc_order != 0) {
        options.max_lpc_order = static_cast<uint8_t>(lpc_order);
    }
    options.lpc_stereo_search = lpc_stereo_search;
    options.max_rice_partition_order = static_cast<uint8_t>(partition_order);
    FLACEncoder encoder(format, options);

    const size_t out_capacity = encoder.get_max_output_bytes();
    uint8_t* out_buffer = static_cast<uint8_t*>(malloc(out_capacity));
    if (!out_buffer) {
        printf("  ERROR: failed to allocate %zu byte output buffer\n", out_capacity);
        return stats;
    }

    // Untimed: the header is written once per stream, not per frame. This
    // first call is also where an unsupported configuration surfaces.
    size_t bytes_written = 0;
    const FLACEncoderResult header_result =
        encoder.write_header(out_buffer, out_capacity, bytes_written);
    if (header_result != FLAC_ENCODER_SUCCESS) {
        printf("  ERROR: write_header() failed with code %d\n", static_cast<int>(header_result));
        free(out_buffer);
        return stats;
    }
    stats.output_bytes += bytes_written;

    const size_t block_bytes = encoder.get_input_block_bytes();

    const uint32_t blocks_per_pass = samples_per_channel / block_size;

    printf("\nRunning: %s (%lu-bit, block size %lu, %lu blocks/pass x %lu passes)\n", label,
           (unsigned long)bits_per_sample, (unsigned long)block_size,
           (unsigned long)blocks_per_pass, (unsigned long)NUM_PASSES);

    for (uint32_t pass = 0; pass < NUM_PASSES; pass++) {
        for (uint32_t block = 0; block < blocks_per_pass; block++) {
            const uint8_t* chunk = input + (static_cast<size_t>(block) * block_bytes);

            size_t bytes_consumed = 0;
            const int64_t start_time = esp_timer_get_time();
            const FLACEncoderResult result = encoder.encode(
                chunk, block_bytes, out_buffer, out_capacity, bytes_consumed, bytes_written);
            const int64_t elapsed = esp_timer_get_time() - start_time;

            if (result != FLAC_ENCODER_SUCCESS) {
                printf("  ERROR: encode() failed with code %d at frame %lu\n",
                       static_cast<int>(result), (unsigned long)stats.frame_count);
                free(out_buffer);
                return stats;
            }

            stats.frame_count++;
            stats.total_time_us += elapsed;
            if (elapsed < stats.min_time_us)
                stats.min_time_us = elapsed;
            if (elapsed > stats.max_time_us)
                stats.max_time_us = elapsed;
            stats.sum_squared += (double)elapsed * elapsed;

            stats.samples_encoded += block_size;
            stats.output_bytes += bytes_written;

            const uint8_t assignment_code = static_cast<uint8_t>(out_buffer[3] >> 4);
            switch (assignment_code) {
                case CHANNEL_ASSIGNMENT_INDEPENDENT:
                    stats.assignment_independent++;
                    break;
                case CHANNEL_ASSIGNMENT_LEFT_SIDE:
                    stats.assignment_left_side++;
                    break;
                case CHANNEL_ASSIGNMENT_RIGHT_SIDE:
                    stats.assignment_right_side++;
                    break;
                case CHANNEL_ASSIGNMENT_MID_SIDE:
                    stats.assignment_mid_side++;
                    break;
                default:
                    break;
            }
        }
    }

    // Whole blocks only, so there is no remainder: this just ends the stream.
    if (encoder.finish(nullptr, 0, out_buffer, out_capacity, bytes_written) !=
        FLAC_ENCODER_SUCCESS) {
        printf("  ERROR: finish() failed\n");
    }

    free(out_buffer);
    return stats;
}

void print_benchmark_results(const EncodeBenchmarkStats& stats) {
    printf("\n--- %s ---\n", stats.label);
    printf("Frames encoded: %lu\n", (unsigned long)stats.frame_count);
    printf("Total encode time: %.2f ms\n", stats.total_time_us / 1000.0);

    if (stats.frame_count == 0)
        return;

    const double avg_time_us = (double)stats.total_time_us / stats.frame_count;
    printf("Per-frame timing:\n");
    printf("  Min: %lld us\n", (long long)stats.min_time_us);
    printf("  Max: %lld us\n", (long long)stats.max_time_us);
    printf("  Avg: %.1f us\n", avg_time_us);

    if (stats.frame_count > 1) {
        const double variance =
            (stats.sum_squared / stats.frame_count) - (avg_time_us * avg_time_us);
        if (variance > 0) {
            printf("  Std: %.1f us\n", std::sqrt(variance));
        }
    }

    // Real-Time Factor: encode wall time vs. the audio duration it covers.
    const double audio_duration_s = (double)stats.samples_encoded / g_sample_rate;
    if (audio_duration_s > 0 && stats.total_time_us > 0) {
        const double encode_duration_s = stats.total_time_us / 1000000.0;
        const double rtf = encode_duration_s / audio_duration_s;
        const double realtime_multiple = 1.0 / rtf;
        printf("RTF: %.4f (%.1fx real-time)\n", rtf, realtime_multiple);
    }

    // Achieved compression: bytes/sample and ratio vs. raw PCM. Each
    // test case is its own stream, so output_bytes includes one 42-byte
    // stream header -- negligible at these sample counts.
    const uint64_t total_samples_all_channels = stats.samples_encoded * stats.num_channels;
    const double bytes_per_sample = total_samples_all_channels > 0
                                        ? (double)stats.output_bytes / total_samples_all_channels
                                        : 0.0;
    printf("Output bytes: %llu (%.1f KB)\n", (unsigned long long)stats.output_bytes,
           stats.output_bytes / 1024.0);
    printf("Bytes/sample: %.3f (%.3f bits/sample)\n", bytes_per_sample, bytes_per_sample * 8.0);
    const double pcm_bytes = stats.bits_per_sample / 8.0;
    printf("Compression ratio vs %lu-bit PCM: %.3f\n", (unsigned long)stats.bits_per_sample,
           bytes_per_sample > 0.0 ? pcm_bytes / bytes_per_sample : 0.0);
    printf("Channel assignment counts: independent=%lu left/side=%lu right/side=%lu mid/side=%lu\n",
           (unsigned long)stats.assignment_independent, (unsigned long)stats.assignment_left_side,
           (unsigned long)stats.assignment_right_side, (unsigned long)stats.assignment_mid_side);
}

void print_summary(const EncodeBenchmarkStats* stats, size_t count) {
    printf("\n");
    printf("================================================================\n");
    printf("                     Benchmark Summary\n");
    printf("================================================================\n\n");
    printf("  %-20s  %10s  %12s  %10s  %10s\n", "Test Case", "Time (ms)", "Real-time", "Bytes/smp",
           "Ratio");
    printf("  %-20s  %10s  %12s  %10s  %10s\n", "--------------------", "----------",
           "------------", "----------", "----------");

    for (size_t i = 0; i < count; i++) {
        const EncodeBenchmarkStats& s = stats[i];
        const double time_ms = s.total_time_us / 1000.0;
        const double audio_duration_s = (double)s.samples_encoded / g_sample_rate;
        double realtime_multiple = 0.0;
        if (audio_duration_s > 0 && s.total_time_us > 0) {
            realtime_multiple = audio_duration_s / (s.total_time_us / 1000000.0);
        }
        const uint64_t total_samples_all_channels = s.samples_encoded * s.num_channels;
        const double bytes_per_sample = total_samples_all_channels > 0
                                            ? (double)s.output_bytes / total_samples_all_channels
                                            : 0.0;
        const double ratio =
            bytes_per_sample > 0.0 ? (s.bits_per_sample / 8.0) / bytes_per_sample : 0.0;

        printf("  %-20s  %10.2f  %11.1fx  %10.3f  %9.3fx\n", s.label, time_ms, realtime_multiple,
               bytes_per_sample, ratio);
    }
    printf("\n");
}

}  // namespace

extern "C" void app_main(void) {
    printf("\n");
    printf("========================================\n");
    printf("   FLAC Encode Benchmark for ESP32\n");
    printf("========================================\n");

    printf("\nDecoding embedded test clip to PCM...\n");
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint32_t samples_per_channel = 0;
    int16_t* signal = decode_test_audio(sample_rate, channels, samples_per_channel);
    if (!signal) {
        printf("ERROR: failed to decode embedded test audio.\n");
        return;
    }
    g_sample_rate = sample_rate;
    g_num_channels = channels;

    const size_t signal_bytes =
        static_cast<size_t>(samples_per_channel) * g_num_channels * sizeof(int16_t);
    printf("Decoded %lu samples/channel, %lu channel(s), %lu Hz (%.1f KB, %.2f s)\n",
           (unsigned long)samples_per_channel, (unsigned long)g_num_channels,
           (unsigned long)g_sample_rate, signal_bytes / 1024.0,
           (double)samples_per_channel / g_sample_rate);

    // Test cases: block sizes from small to large; LPC at orders 4, 8 and 12
    // and with the stereo search; "r6" cases with Rice partitioning up to
    // order 6. A build without a feature runs its cases without it.
    //
    // The 24-bit cases encode the clip widened into a second PSRAM buffer,
    // exercising the 3-byte unpacking. The mono case runs last: it compacts that
    // buffer's left channel in place, since PSRAM has no room for a third
    // copy.
    struct TestCase {
        const char* label;
        uint32_t bits_per_sample;
        uint32_t num_channels;
        uint32_t block_size;
        uint32_t lpc_order;
        bool lpc_stereo_search;
        uint32_t partition_order;
    };
    static const TestCase test_cases[] = {
        {"4096 (large)", 16, 2, 4096, 0, false, 0},
        {"1152 (typical)", 16, 2, 1152, 0, false, 0},
        {"576 (small)", 16, 2, 576, 0, false, 0},
        {"192 (tiny)", 16, 2, 192, 0, false, 0},
        {"LPC-4 4096", 16, 2, 4096, 4, false, 0},
        {"LPC-8 4096", 16, 2, 4096, 8, false, 0},
        {"LPC-8 1152", 16, 2, 1152, 8, false, 0},
        {"LPC-12 4096", 16, 2, 4096, 12, false, 0},
        {"LPC-8 search 4096", 16, 2, 4096, 8, true, 0},
        {"LPC-8 search 1152", 16, 2, 1152, 8, true, 0},
        {"4096 r6", 16, 2, 4096, 0, false, 6},
        {"1152 r6", 16, 2, 1152, 0, false, 6},
        {"LPC-8 r6 4096", 16, 2, 4096, 8, false, 6},
        {"LPC-8 search r6 4096", 16, 2, 4096, 8, true, 6},
        {"24b 4096 (large)", 24, 2, 4096, 0, false, 0},
        {"24b 1152 (typical)", 24, 2, 1152, 0, false, 0},
        {"24b 4096 r6", 24, 2, 4096, 0, false, 6},
        {"24b LPC-8 4096", 24, 2, 4096, 8, false, 0},
        {"24b mono 4096", 24, 1, 4096, 0, false, 0},
    };
    constexpr size_t num_tests = sizeof(test_cases) / sizeof(test_cases[0]);

    const size_t total_samples = static_cast<size_t>(samples_per_channel) * g_num_channels;
    printf("PSRAM: %zu KB total, %zu KB free\n", heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024,
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    uint8_t* signal24 = static_cast<uint8_t*>(
        heap_caps_malloc(total_samples * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (signal24) {
        widen_to_24bit(signal24, signal, total_samples);
    } else {
        printf("WARNING: no room for the %.1f KB 24-bit clip; skipping the 24-bit cases\n",
               total_samples * 3 / 1024.0);
    }

    EncodeBenchmarkStats results[num_tests];
    size_t num_run = 0;
    bool mono24_ready = false;
    for (size_t i = 0; i < num_tests; i++) {
        const TestCase& tc = test_cases[i];
        const uint8_t* input = reinterpret_cast<const uint8_t*>(signal);
        if (tc.bits_per_sample == 24) {
            input = signal24;
            if (signal24 != nullptr && tc.num_channels == 1 && !mono24_ready) {
                // Frame i's left sample moves from byte 3 * i * channels to
                // 3 * i, never forward of where it is read from.
                for (size_t f = 0; f < samples_per_channel; f++) {
                    memmove(signal24 + (3 * f), signal24 + (3 * f * g_num_channels), 3);
                }
                mono24_ready = true;
            }
        }
        if (input == nullptr) {
            continue;
        }
        results[num_run] = run_encode_benchmark(
            input, samples_per_channel, tc.label, tc.bits_per_sample, tc.num_channels,
            tc.block_size, tc.lpc_order, tc.lpc_stereo_search, tc.partition_order);
        print_benchmark_results(results[num_run]);
        num_run++;
    }

    print_summary(results, num_run);
    heap_caps_free(signal24);

    heap_caps_free(signal);
    printf("Benchmark complete.\n");
}
