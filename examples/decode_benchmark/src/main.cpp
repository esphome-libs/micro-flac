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
 * FLAC Decode Benchmark for ESP32
 *
 * Measures FLAC decoding performance including:
 * - Per-frame decode timing (min/max/avg/stddev)
 * - Total decode time
 * - Real-Time Factor (RTF)
 *
 * Audio source: Public domain music (Beethoven's Eroica from Musopen)
 */

#include "esp_timer.h"
#include "micro_flac/flac_decoder.h"
#include "test_audio_flac.h"
#ifdef BENCHMARK_INCLUDE_24BIT
#include "test_audio_flac_24bit.h"
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace micro_flac;

// Statistics tracking
struct BenchmarkStats {
    const char* label = "";
    size_t chunk_size = 0;  // 0 = full frame
    uint32_t frame_count = 0;
    int64_t total_time_us = 0;
    int64_t min_time_us = INT64_MAX;
    int64_t max_time_us = 0;
    double sum_squared = 0.0;  // For standard deviation calculation

    // Stream info captured from the decoder after header parsing
    uint32_t sample_rate = 0;
    uint32_t num_channels = 0;
    uint32_t bits_per_sample = 0;
    uint32_t max_block_size = 0;
    uint64_t num_samples = 0;
};

static void print_stream_info(const BenchmarkStats& stats) {
    printf("\n=== FLAC Stream Info ===\n");
    printf("Sample rate: %lu Hz\n", (unsigned long)stats.sample_rate);
    printf("Channels: %lu\n", (unsigned long)stats.num_channels);
    printf("Bit depth: %lu\n", (unsigned long)stats.bits_per_sample);
    printf("Max block size: %lu samples\n", (unsigned long)stats.max_block_size);

    if (stats.num_samples > 0) {
        double duration = (double)stats.num_samples / stats.sample_rate;
        printf("Total samples: %llu\n", (unsigned long long)stats.num_samples);
        printf("Audio duration: %.2f seconds\n", duration);
    } else {
        printf("Total samples: unknown\n");
    }
    printf("\n");
}

static void print_benchmark_results(BenchmarkStats& stats) {
    printf("\n--- %s ---\n", stats.label);
    printf("Frames decoded: %lu\n", (unsigned long)stats.frame_count);
    printf("Total decode time: %.2f ms\n", stats.total_time_us / 1000.0);

    if (stats.frame_count > 0) {
        double avg_time_us = (double)stats.total_time_us / stats.frame_count;

        printf("Per-frame timing:\n");
        printf("  Min: %lld us\n", (long long)stats.min_time_us);
        printf("  Max: %lld us\n", (long long)stats.max_time_us);
        printf("  Avg: %.1f us\n", avg_time_us);

        // Calculate standard deviation
        if (stats.frame_count > 1) {
            double variance = (stats.sum_squared / stats.frame_count) - (avg_time_us * avg_time_us);
            if (variance > 0) {
                double stddev = sqrt(variance);
                printf("  Std: %.1f us\n", stddev);
            }
        }
    }

    // Calculate Real-Time Factor
    if (stats.num_samples > 0 && stats.total_time_us > 0) {
        double audio_duration_s = (double)stats.num_samples / stats.sample_rate;
        double decode_duration_s = stats.total_time_us / 1000000.0;
        double rtf = decode_duration_s / audio_duration_s;
        double speedup = 1.0 / rtf;

        printf("RTF: %.4f (%.1fx real-time)\n", rtf, speedup);
    }
}

// Run a single decode benchmark pass using the unified decode() API
// chunk_size = 0 means pass the full remaining buffer each time (full frame mode)
static BenchmarkStats run_decode_pass(const uint8_t* data, size_t data_len, const char* label,
                                      size_t chunk_size, bool enable_crc,
                                      bool output_32bit = false) {
    BenchmarkStats stats;
    stats.label = label;
    stats.chunk_size = chunk_size;

    // Create a fresh decoder for each pass
    FLACDecoder decoder;
    decoder.set_crc_check_enabled(enable_crc);

    uint8_t* output_buffer = nullptr;
    int32_t* output_buffer_32 = nullptr;
    size_t output_buffer_size = 0;
    size_t output_buffer_samples = 0;

    const uint8_t* input_ptr = data;
    size_t bytes_remaining = data_len;

    printf("\nRunning: %s\n", label);

    int64_t frame_start_time = 0;

    while (bytes_remaining > 0) {
        size_t consumed = 0;
        size_t samples_decoded = 0;

        size_t pass_size =
            (chunk_size == 0) ? bytes_remaining : std::min(bytes_remaining, chunk_size);

        FLACDecoderResult result;
        if (output_32bit) {
            result = decoder.decode(input_ptr, pass_size, output_buffer_32, output_buffer_samples,
                                    consumed, samples_decoded);
        } else {
            result = decoder.decode(input_ptr, pass_size, output_buffer, output_buffer_size,
                                    consumed, samples_decoded);
        }
        input_ptr += consumed;
        bytes_remaining -= consumed;

        if (result == FLAC_DECODER_HEADER_READY) {
            // Capture stream info
            const FLACStreamInfo& stream_info = decoder.get_stream_info();
            stats.sample_rate = stream_info.sample_rate();
            stats.num_channels = stream_info.num_channels();
            stats.bits_per_sample = stream_info.bits_per_sample();
            stats.max_block_size = stream_info.max_block_size();
            stats.num_samples = stream_info.total_samples_per_channel();

            // Allocate output buffer
            output_buffer_samples = decoder.get_output_buffer_size_samples();
            if (output_32bit) {
                output_buffer_32 = (int32_t*)malloc(output_buffer_samples * sizeof(int32_t));
                if (!output_buffer_32) {
                    printf("ERROR: Failed to allocate output buffer\n");
                    return stats;
                }
            } else {
                uint32_t bytes_per_sample = stream_info.bytes_per_sample();
                output_buffer_size = output_buffer_samples * bytes_per_sample;
                output_buffer = (uint8_t*)malloc(output_buffer_size);
                if (!output_buffer) {
                    printf("ERROR: Failed to allocate output buffer\n");
                    return stats;
                }
            }

            // Start timing from the first frame
            frame_start_time = esp_timer_get_time();
        } else if (result == FLAC_DECODER_SUCCESS) {
            int64_t end_time = esp_timer_get_time();
            int64_t frame_time = end_time - frame_start_time;

            stats.frame_count++;
            stats.total_time_us += frame_time;
            if (frame_time < stats.min_time_us)
                stats.min_time_us = frame_time;
            if (frame_time > stats.max_time_us)
                stats.max_time_us = frame_time;
            stats.sum_squared += (double)frame_time * frame_time;

            // Reset timer for next frame
            frame_start_time = esp_timer_get_time();
        } else if (result == FLAC_DECODER_END_OF_STREAM) {
            printf("  End of stream reached.\n");
            break;
        } else if (result == FLAC_DECODER_NEED_MORE_DATA) {
            // Continue feeding data
        } else {
            printf("  ERROR: Decode failed with error %d at frame %lu\n", result,
                   (unsigned long)stats.frame_count);
            break;
        }
    }

    free(output_buffer);
    free(output_buffer_32);
    return stats;
}

static void print_summary(BenchmarkStats* no_crc_stats, BenchmarkStats* crc_stats, size_t count) {
    printf("\n");
    printf("================================================================\n");
    printf("                     Benchmark Summary\n");
    printf("================================================================\n");

    // Use stream info from first stats entry
    uint64_t total_samples = no_crc_stats[0].num_samples;
    uint32_t sample_rate = no_crc_stats[0].sample_rate;
    double audio_duration_s = 0.0;
    if (total_samples > 0) {
        audio_duration_s = (double)total_samples / sample_rate;
    }

    printf("\n  %-20s  %20s  %20s\n", "", "CRC Disabled", "CRC Enabled");
    printf("  %-20s  %10s %9s  %10s %9s\n", "Test Case", "Time (ms)", "Real-time", "Time (ms)",
           "Real-time");
    printf("  %-20s  %10s %9s  %10s %9s\n", "--------------------", "----------", "---------",
           "----------", "---------");

    for (size_t i = 0; i < count; i++) {
        double no_crc_ms = no_crc_stats[i].total_time_us / 1000.0;
        double crc_ms = crc_stats[i].total_time_us / 1000.0;
        double no_crc_rt = 0.0;
        double crc_rt = 0.0;
        if (audio_duration_s > 0) {
            if (no_crc_stats[i].total_time_us > 0)
                no_crc_rt = audio_duration_s / (no_crc_stats[i].total_time_us / 1000000.0);
            if (crc_stats[i].total_time_us > 0)
                crc_rt = audio_duration_s / (crc_stats[i].total_time_us / 1000000.0);
        }
        printf("  %-20s  %10.2f %8.1fx  %10.2f %8.1fx\n", no_crc_stats[i].label, no_crc_ms,
               no_crc_rt, crc_ms, crc_rt);
    }

    printf("\n");
}

// Define test cases: {label, chunk_size}
// chunk_size = 0 means pass the full remaining buffer (full frame mode)
struct TestCase {
    const char* label;
    size_t chunk_size;
};

static const TestCase test_cases[] = {
    {"Full frame", 0},        {"1000 byte chunks", 1000}, {"500 byte chunks", 500},
    {"100 byte chunks", 100}, {"4 byte chunks", 4},       {"1 byte chunks", 1},
};

static constexpr size_t num_tests = sizeof(test_cases) / sizeof(test_cases[0]);

static void run_benchmark_suite(const char* suite_name, const uint8_t* data, size_t data_len,
                                bool output_32bit = false) {
    printf("\n");
    printf("========================================\n");
    printf("   %s\n", suite_name);
    printf("========================================\n");

    printf("Input data size: %lu bytes (%.1f KB)\n", (unsigned long)data_len, data_len / 1024.0);

    BenchmarkStats no_crc_stats[num_tests];
    BenchmarkStats crc_stats[num_tests];

    // Run all tests without CRC
    printf("\n========================================\n");
    printf("   CRC Disabled\n");
    printf("========================================\n");
    for (size_t i = 0; i < num_tests; i++) {
        no_crc_stats[i] = run_decode_pass(data, data_len, test_cases[i].label,
                                          test_cases[i].chunk_size, false, output_32bit);
        if (i == 0) {
            print_stream_info(no_crc_stats[0]);
        }
        print_benchmark_results(no_crc_stats[i]);
    }

    // Run all tests with CRC
    printf("\n========================================\n");
    printf("   CRC Enabled\n");
    printf("========================================\n");
    for (size_t i = 0; i < num_tests; i++) {
        crc_stats[i] = run_decode_pass(data, data_len, test_cases[i].label,
                                       test_cases[i].chunk_size, true, output_32bit);
        print_benchmark_results(crc_stats[i]);
    }

    print_summary(no_crc_stats, crc_stats, num_tests);

    if (no_crc_stats[0].num_samples > 0) {
        printf("Audio duration: %.2f seconds\n",
               (double)no_crc_stats[0].num_samples / no_crc_stats[0].sample_rate);
    }
}

extern "C" void app_main(void) {
    printf("\n");
    printf("========================================\n");
    printf("   FLAC Decode Benchmark for ESP32\n");
    printf("========================================\n");

    run_benchmark_suite("16-bit / 48 kHz", test_audio_flac_data, test_audio_flac_data_len);
#ifdef BENCHMARK_INCLUDE_24BIT
    run_benchmark_suite("24-bit / 48 kHz", test_audio_flac_24bit_data,
                        test_audio_flac_24bit_data_len);
    run_benchmark_suite("24-bit / 48 kHz (32-bit output)", test_audio_flac_24bit_data,
                        test_audio_flac_24bit_data_len, true);
#endif
    printf("Benchmark complete.\n");
}
