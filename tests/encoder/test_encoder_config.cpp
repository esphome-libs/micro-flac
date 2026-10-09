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

/// @file test_encoder_config.cpp
/// @brief Unit test for FLACEncoder's API contract, validation, and odd depths
///
/// Covers:
///
/// 1. The result codes every call returns for unsupported configurations and
///    bad arguments, and the encode()/finish()/write_header()/reset() stream
///    lifecycle.
/// 2. Internal state the public API deliberately hides: the frame-number
///    limit, and the stereo estimate's size win over forced independent
///    coding. This file is built with
///    -fno-access-control (see CMakeLists.txt) so it can read and set
///    FLACEncoder's private members directly, which keeps test-only hooks out
///    of the public header.
/// 3. Round-trip fidelity at bit depths a WAV file cannot carry. WAV stores
///    only whole-byte sample widths, so depths like 12, 17 and 20 are
///    unreachable through WAV files even though the format and this encoder
///    both support them. These are encoded and decoded entirely in memory,
///    and the decoder's byte output must equal the encoder's byte input.
///
/// Build/run (from tests/encoder):
///   cmake -B build && cmake --build build
///   ./build/test_encoder_config

#include "micro_flac/flac_decoder.h"
#include "micro_flac/flac_encoder.h"
#include "micro_flac/pcm_format.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

// Test vectors, depths, block sizes and expected sizes are literal throughout;
// naming each would only obscure what is being checked.
// NOLINTBEGIN(readability-magic-numbers)

namespace {

using namespace micro_flac;

constexpr uint32_t RATE = 44100;
constexpr uint32_t BLOCK = 4096;

int failures = 0;

void check(const char* what, long long got, long long want) {
    const bool ok = (got == want);
    if (!ok) {
        failures++;
    }
    std::printf("  %-60s got=%-6lld want=%-6lld %s\n", what, got, want, ok ? "OK" : "<-- FAIL");
}

void check_true(const char* what, bool ok) {
    if (!ok) {
        failures++;
    }
    std::printf("  %-60s %s\n", what, ok ? "OK" : "<-- FAIL");
}

PcmFormat make_format(uint32_t channels, uint32_t bits, uint32_t rate = RATE) {
    return PcmFormat(rate, channels, bits);
}

FLACEncoderOptions make_options(uint32_t block_size) {
    FLACEncoderOptions options;
    options.block_size = block_size;
    return options;
}

// Result of the first call on a fresh encoder, which is where the
// constructor's verdict on the configuration surfaces.
FLACEncoderResult first_use(const PcmFormat& format, const FLACEncoderOptions& options) {
    FLACEncoder e(format, options);
    std::vector<uint8_t> out(FLACEncoder::HEADER_BYTES);
    size_t written = 0;
    return e.write_header(out.data(), out.size(), written);
}

// ============================================================================
// Configuration validation
// ============================================================================

void check_config_validation() {
    std::printf("Configuration is validated at construction, reported on first use:\n");

    const FLACEncoderOptions defaults = make_options(BLOCK);

    check("bits_per_sample 25 (above max)", first_use(make_format(2, 25), defaults),
          FLAC_ENCODER_ERROR_BAD_CONFIG);
    check("bits_per_sample 3 (below min)", first_use(make_format(2, 3), defaults),
          FLAC_ENCODER_ERROR_BAD_CONFIG);
    check("bits_per_sample 4 (min)", first_use(make_format(1, 4), defaults), FLAC_ENCODER_SUCCESS);
    check("bits_per_sample 24 + 8 channels (max)", first_use(make_format(8, 24), defaults),
          FLAC_ENCODER_SUCCESS);
    check("16-bit stereo", first_use(make_format(2, 16), defaults), FLAC_ENCODER_SUCCESS);
    check("16-bit 3 channels", first_use(make_format(3, 16), defaults), FLAC_ENCODER_SUCCESS);
    check("num_channels 9 (above max)", first_use(make_format(9, 16), defaults),
          FLAC_ENCODER_ERROR_BAD_CONFIG);
    // Past uint8_t: PcmFormat stores 0 rather than wrapping 257 to a valid 1
    check("num_channels 257 (would wrap to 1)", first_use(make_format(257, 16), defaults),
          FLAC_ENCODER_ERROR_BAD_CONFIG);
    check("bits_per_sample 272 (would wrap to 16)", first_use(make_format(2, 272), defaults),
          FLAC_ENCODER_ERROR_BAD_CONFIG);
    check("num_channels 0", first_use(make_format(0, 16), defaults), FLAC_ENCODER_ERROR_BAD_CONFIG);
    check("block_size 15 (below min)", first_use(make_format(2, 16), make_options(15)),
          FLAC_ENCODER_ERROR_BAD_CONFIG);
    check("block_size 16 (min)", first_use(make_format(2, 16), make_options(16)),
          FLAC_ENCODER_SUCCESS);
    check("block_size 65535 (max)", first_use(make_format(2, 16), make_options(65535)),
          FLAC_ENCODER_SUCCESS);
    check("block_size 65536 (above max)", first_use(make_format(2, 16), make_options(65536)),
          FLAC_ENCODER_ERROR_BAD_CONFIG);
    {
        PcmFormat format = make_format(2, 16, 0);
        check("sample_rate 0", first_use(format, defaults), FLAC_ENCODER_ERROR_BAD_CONFIG);
        format = make_format(2, 16, 655350);
        check("sample_rate 655350 (max, tens-of-Hz code)", first_use(format, defaults),
              FLAC_ENCODER_SUCCESS);
        // A rate no frame header code can carry is read from STREAMINFO, whose
        // 20-bit field sets the maximum
        format = make_format(2, 16, 65537);
        check("sample_rate 65537 (from STREAMINFO)", first_use(format, defaults),
              FLAC_ENCODER_SUCCESS);
        format = make_format(2, 16, 1048575);
        check("sample_rate 1048575 (max, STREAMINFO's 20 bits)", first_use(format, defaults),
              FLAC_ENCODER_SUCCESS);
        format = make_format(2, 16, 1048576);
        check("sample_rate 1048576 (above max)", first_use(format, defaults),
              FLAC_ENCODER_ERROR_BAD_CONFIG);
    }

    {
        // get_options() hands back the options as passed, even ones a build ignores
        FLACEncoderOptions options = make_options(1152);
        options.wasted_bits = false;
        const FLACEncoder e(make_format(2, 16), options);
        const FLACEncoderOptions& got = e.get_options();
        check_true("get_options() returns the options passed",
                   got.block_size == 1152 && !got.wasted_bits);
    }

    std::printf("\nUnsupported configurations fail every call until reconfigured:\n");
    {
        FLACEncoder e(make_format(9, 16));
        check("get_max_output_bytes() is 0", static_cast<long long>(e.get_max_output_bytes()), 0);
        check("get_input_block_bytes() is 0", static_cast<long long>(e.get_input_block_bytes()), 0);
        std::vector<uint8_t> buf(64);
        size_t consumed = 1;
        size_t written = 1;
        check("encode()",
              e.encode(buf.data(), buf.size(), buf.data(), buf.size(), consumed, written),
              FLAC_ENCODER_ERROR_BAD_CONFIG);
        check("  and consumes/writes nothing",
              static_cast<long long>(consumed) + static_cast<long long>(written), 0);
        check("finish()", e.finish(buf.data(), 0, buf.data(), buf.size(), written),
              FLAC_ENCODER_ERROR_BAD_CONFIG);
        check("write_header(), repeated", e.write_header(buf.data(), buf.size(), written),
              FLAC_ENCODER_ERROR_BAD_CONFIG);
    }
}

// ============================================================================
// No allocation
// ============================================================================

void check_no_allocation() {
    std::printf("\nEvery configuration works without allocating:\n");

    // The work buffer lives in the object, so no format needs an allocation.
    const FLACEncoderOptions options = make_options(BLOCK);
    check("6 channels", first_use(make_format(6, 16), options), FLAC_ENCODER_SUCCESS);
    check("24-bit stereo", first_use(make_format(2, 24), options), FLAC_ENCODER_SUCCESS);
    check("12-bit mono (left-justified, needs unpacking)", first_use(make_format(1, 12), options),
          FLAC_ENCODER_SUCCESS);
    check("8-bit mono (1-byte samples)", first_use(make_format(1, 8), options),
          FLAC_ENCODER_SUCCESS);
}

// ============================================================================
// encode() / finish() arguments and stream lifecycle
// ============================================================================

void check_encode_arguments() {
    std::printf("\nencode() arguments:\n");

    // 24-bit 6-channel: 3-byte samples, no alignment rule.
    const uint32_t arg_channels = 6;
    const uint32_t arg_bytes = 3;
    FLACEncoder e(make_format(arg_channels, arg_bytes * 8), make_options(BLOCK));
    const size_t block_bytes = e.get_input_block_bytes();
    check("get_input_block_bytes() = block_size * bytes * channels",
          static_cast<long long>(block_bytes),
          static_cast<long long>(BLOCK) * arg_bytes * arg_channels);

    // One spare byte in front, so input + 1 is an odd address.
    std::vector<uint8_t> in(block_bytes * 2 + 1, 0);
    std::vector<uint8_t> out(e.get_max_output_bytes());
    size_t consumed = 12345;  // Poisoned, so the "zeroed on error" checks are meaningful
    size_t written = 12345;

    check("null output", e.encode(in.data(), block_bytes, nullptr, out.size(), consumed, written),
          FLAC_ENCODER_ERROR_INVALID_ARGUMENT);
    check("  zeroes bytes_consumed and bytes_written",
          static_cast<long long>(consumed) + static_cast<long long>(written), 0);
    check("null input with bytes", e.encode(nullptr, 1, out.data(), out.size(), consumed, written),
          FLAC_ENCODER_ERROR_INVALID_ARGUMENT);
    check("null input with 0 bytes is just too little",
          e.encode(nullptr, 0, out.data(), out.size(), consumed, written),
          FLAC_ENCODER_NEED_MORE_DATA);
    check("one byte short of a block",
          e.encode(in.data(), block_bytes - 1, out.data(), out.size(), consumed, written),
          FLAC_ENCODER_NEED_MORE_DATA);
    check("  consumes nothing", static_cast<long long>(consumed), 0);
    // NEED_MORE_DATA outranks the output check: without a full block there
    // is nothing to write, so the output buffer is irrelevant.
    check("short input + undersized output",
          e.encode(in.data(), block_bytes - 1, out.data(), 1, consumed, written),
          FLAC_ENCODER_NEED_MORE_DATA);
    // One byte under the advertised bound must be rejected up front, which is
    // what makes get_max_output_bytes() safe to size buffers with.
    check("output one byte under get_max_output_bytes()",
          e.encode(in.data(), block_bytes, out.data(), out.size() - 1, consumed, written),
          FLAC_ENCODER_ERROR_OUTPUT_BUFFER_TOO_SMALL);
    check("  consumes nothing", static_cast<long long>(consumed), 0);
    check_true("  none of those calls advanced the stream", e.get_total_samples_encoded() == 0 &&
                                                                e.frame_number_ == 0 &&
                                                                e.max_frame_bytes_ == 0);
    check("odd address is fine for 3-byte samples",
          e.encode(in.data() + 1, block_bytes, out.data(), out.size(), consumed, written),
          FLAC_ENCODER_SUCCESS);
    check("more than a block consumes exactly one block",
          e.encode(in.data(), in.size(), out.data(), out.size(), consumed, written),
          FLAC_ENCODER_SUCCESS);
    check("  bytes_consumed", static_cast<long long>(consumed),
          static_cast<long long>(block_bytes));
    check_true("  bytes_written is a frame", written > 0 && written <= out.size());

    std::printf("\n2-byte samples must be 2-byte aligned:\n");
    for (const uint32_t bits : {16U, 12U}) {
        FLACEncoder e2(make_format(2, bits));
        std::vector<uint8_t> in2(e2.get_input_block_bytes() + 1, 0);
        std::vector<uint8_t> out2(e2.get_max_output_bytes());
        char label[64];
        std::snprintf(label, sizeof(label), "%u-bit stereo from an odd address", bits);
        check(label,
              e2.encode(in2.data() + 1, e2.get_input_block_bytes(), out2.data(), out2.size(),
                        consumed, written),
              FLAC_ENCODER_ERROR_INVALID_ARGUMENT);
        std::snprintf(label, sizeof(label), "%u-bit stereo from an even address", bits);
        check(label,
              e2.encode(in2.data(), e2.get_input_block_bytes(), out2.data(), out2.size(), consumed,
                        written),
              FLAC_ENCODER_SUCCESS);
    }
}

void check_finish_and_lifecycle() {
    std::printf("\nfinish() and the stream lifecycle:\n");

    FLACEncoder e(make_format(2, 16), make_options(BLOCK));
    const size_t block_bytes = e.get_input_block_bytes();
    const size_t frame_bytes = e.get_pcm_format().bytes_per_frame();
    std::vector<uint8_t> in(block_bytes * 2, 0);
    std::vector<uint8_t> out(e.get_max_output_bytes());
    size_t consumed = 0;
    size_t written = 12345;

    check("remainder not a whole sample frame",
          e.finish(in.data(), frame_bytes + 1, out.data(), out.size(), written),
          FLAC_ENCODER_ERROR_INVALID_ARGUMENT);
    check("  writes nothing", static_cast<long long>(written), 0);
    check("remainder longer than one block",
          e.finish(in.data(), block_bytes + frame_bytes, out.data(), out.size(), written),
          FLAC_ENCODER_ERROR_INVALID_ARGUMENT);
    check("encoding continues after those errors",
          e.encode(in.data(), block_bytes, out.data(), out.size(), consumed, written),
          FLAC_ENCODER_SUCCESS);
    check("a full block is an acceptable remainder",
          e.finish(in.data(), block_bytes, out.data(), out.size(), written), FLAC_ENCODER_SUCCESS);
    check_true("  and writes a frame", written > 0);
    check("encode() after finish()",
          e.encode(in.data(), block_bytes, out.data(), out.size(), consumed, written),
          FLAC_ENCODER_ERROR_STREAM_FINISHED);
    check("finish() after finish()", e.finish(nullptr, 0, out.data(), out.size(), written),
          FLAC_ENCODER_ERROR_STREAM_FINISHED);
    check("write_header() after finish()", e.write_header(out.data(), out.size(), written),
          FLAC_ENCODER_SUCCESS);
    check("get_total_samples_encoded()", static_cast<long long>(e.get_total_samples_encoded()),
          2LL * BLOCK);

    e.reset();
    check("reset() clears the sample count", static_cast<long long>(e.get_total_samples_encoded()),
          0);
    check("encode() after reset()",
          e.encode(in.data(), block_bytes, out.data(), out.size(), consumed, written),
          FLAC_ENCODER_SUCCESS);
    check("finish() with nothing left over", e.finish(nullptr, 0, out.data(), out.size(), written),
          FLAC_ENCODER_SUCCESS);
    check("  writes nothing", static_cast<long long>(written), 0);

    std::printf("\nFrame number limit (private frame_number_ set via -fno-access-control):\n");
    {
        FLACEncoder big(make_format(1, 16), make_options(16));
        std::vector<uint8_t> in16(big.get_input_block_bytes(), 0);
        std::vector<uint8_t> out16(big.get_max_output_bytes());
        big.frame_number_ = (1UL << 31) - 1;
        check("frame number 2^31 - 1",
              big.encode(in16.data(), in16.size(), out16.data(), out16.size(), consumed, written),
              FLAC_ENCODER_SUCCESS);
        static const uint8_t CODED[6] = {0xFD, 0xBF, 0xBF, 0xBF, 0xBF, 0xBF};
        check_true("  coded as the 6-byte form FD BF BF BF BF BF",
                   std::memcmp(out16.data() + 4, CODED, sizeof(CODED)) == 0);
        check("frame number 2^31",
              big.encode(in16.data(), in16.size(), out16.data(), out16.size(), consumed, written),
              FLAC_ENCODER_ERROR_STREAM_TOO_LONG);
        check("finish() with no remainder still succeeds",
              big.finish(nullptr, 0, out16.data(), out16.size(), written), FLAC_ENCODER_SUCCESS);
    }
}

// ============================================================================
// Stream header
// ============================================================================

uint64_t streaminfo_total_samples(const uint8_t* h) {
    const uint8_t* si = h + 8;
    return (static_cast<uint64_t>(si[13] & 0x0FU) << 32) | (static_cast<uint64_t>(si[14]) << 24) |
           (static_cast<uint64_t>(si[15]) << 16) | (static_cast<uint64_t>(si[16]) << 8) | si[17];
}

uint32_t streaminfo_u24(const uint8_t* h, size_t offset) {
    const uint8_t* si = h + 8 + offset;
    return (static_cast<uint32_t>(si[0]) << 16) | (static_cast<uint32_t>(si[1]) << 8) | si[2];
}

void check_header() {
    std::printf("\nwrite_header():\n");

    FLACEncoder e(make_format(2, 16), make_options(1000));
    std::vector<uint8_t> in(e.get_input_block_bytes() * 3, 0);
    for (size_t i = 0; i < in.size(); i++) {
        in[i] = static_cast<uint8_t>((i * 7) ^ (i >> 5));  // Not all frames the same size
    }
    std::vector<uint8_t> out(e.get_max_output_bytes());
    std::vector<uint8_t> first(FLACEncoder::HEADER_BYTES);
    std::vector<uint8_t> later(FLACEncoder::HEADER_BYTES);
    size_t written = 0;
    size_t consumed = 0;

    check("buffer one byte under HEADER_BYTES",
          e.write_header(first.data(), FLACEncoder::HEADER_BYTES - 1, written),
          FLAC_ENCODER_ERROR_OUTPUT_BUFFER_TOO_SMALL);
    check("null output", e.write_header(nullptr, FLACEncoder::HEADER_BYTES, written),
          FLAC_ENCODER_ERROR_INVALID_ARGUMENT);
    check("provisional header", e.write_header(first.data(), first.size(), written),
          FLAC_ENCODER_SUCCESS);
    check("  is HEADER_BYTES long", static_cast<long long>(written),
          static_cast<long long>(FLACEncoder::HEADER_BYTES));
    check_true("  starts with fLaC", std::memcmp(first.data(), "fLaC", 4) == 0);

    size_t min_frame = SIZE_MAX;
    size_t max_frame = 0;
    const uint8_t* p = in.data();
    size_t remaining =
        in.size() - (5 * static_cast<size_t>(e.get_pcm_format().bytes_per_frame()));  // short tail
    while (e.encode(p, remaining, out.data(), out.size(), consumed, written) ==
           FLAC_ENCODER_SUCCESS) {
        min_frame = (written < min_frame) ? written : min_frame;
        max_frame = (written > max_frame) ? written : max_frame;
        p += consumed;
        remaining -= consumed;
    }

    check("mid-stream header", e.write_header(later.data(), later.size(), written),
          FLAC_ENCODER_SUCCESS);
    check_true("  is identical to the provisional one", first == later);

    check("finish() the short tail", e.finish(p, remaining, out.data(), out.size(), written),
          FLAC_ENCODER_SUCCESS);
    min_frame = (written < min_frame) ? written : min_frame;
    max_frame = (written > max_frame) ? written : max_frame;
    check("finished header", e.write_header(later.data(), later.size(), written),
          FLAC_ENCODER_SUCCESS);
    check("  is HEADER_BYTES long", static_cast<long long>(written),
          static_cast<long long>(FLACEncoder::HEADER_BYTES));
    check("  total samples", static_cast<long long>(streaminfo_total_samples(later.data())),
          3LL * 1000 - 5);
    check("  min frame size", streaminfo_u24(later.data(), 4), static_cast<long long>(min_frame));
    check("  max frame size", streaminfo_u24(later.data(), 7), static_cast<long long>(max_frame));
    check_true("  provisional header left those unknown",
               streaminfo_total_samples(first.data()) == 0 &&
                   streaminfo_u24(first.data(), 4) == 0 && streaminfo_u24(first.data(), 7) == 0);
    check_true("  and differs from the finished one only in those fields",
               std::memcmp(first.data(), later.data(), 12) == 0 &&
                   std::memcmp(first.data() + 18, later.data() + 18, 3) == 0 &&
                   (first[21] & 0xF0U) == (later[21] & 0xF0U) &&
                   std::memcmp(first.data() + 26, later.data() + 26, 16) == 0);
}

// ============================================================================
// Stereo estimation
// ============================================================================

// Encode `in` (whole stream) and return the total bytes produced, with the
// four-way stereo estimate on or forced off through the private
// stereo_estimation_enabled_ flag.
size_t encoded_size(const PcmFormat& format, const std::vector<uint8_t>& in, bool estimation) {
    FLACEncoder e(format, make_options(BLOCK));
    e.stereo_estimation_enabled_ = estimation;
    std::vector<uint8_t> out(e.get_max_output_bytes());
    size_t total = 0;
    size_t consumed = 0;
    size_t written = 0;
    const uint8_t* p = in.data();
    size_t remaining = in.size();
    while (e.encode(p, remaining, out.data(), out.size(), consumed, written) ==
           FLAC_ENCODER_SUCCESS) {
        total += written;
        p += consumed;
        remaining -= consumed;
    }
    if (e.finish(p, remaining, out.data(), out.size(), written) != FLAC_ENCODER_SUCCESS) {
        return 0;
    }
    return total + written;
}

void check_stereo_estimation() {
    std::printf("\nStereo estimation beats forced independent coding on L == R input\n");
    std::printf("(private stereo_estimation_enabled_ set via -fno-access-control):\n");

    // With L == R the side channel is identically zero and collapses to a
    // CONSTANT subframe, so the estimate should roughly halve the output.
    for (uint32_t bits = 16; bits <= 24; bits += 8) {
        const PcmFormat format = make_format(2, bits);
        const size_t bytes = format.bytes_per_sample();
        const uint32_t n = BLOCK * 8;
        std::vector<uint8_t> in(static_cast<size_t>(n) * 2 * bytes);
        uint32_t state = 0x2468ACE1U;
        int32_t v = 0;
        const int32_t limit = 1 << (bits - 3);
        for (uint32_t i = 0; i < n; i++) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            v += static_cast<int32_t>(state % 64U) - 32;  // Correlated random walk
            v = (v > limit) ? limit : ((v < -limit) ? -limit : v);
            for (uint32_t c = 0; c < 2; c++) {
                const uint32_t u = static_cast<uint32_t>(v);
                for (size_t b = 0; b < bytes; b++) {
                    in[(static_cast<size_t>(i) * 2 + c) * bytes + b] =
                        static_cast<uint8_t>(u >> (8 * b));
                }
            }
        }
        const size_t with = encoded_size(format, in, true);
        const size_t without = encoded_size(format, in, false);
        const double ratio =
            without ? static_cast<double>(with) / static_cast<double>(without) : 1.0;
        char label[96];
        std::snprintf(label, sizeof(label), "%u-bit: %zu vs %zu bytes, ratio %.3f (< 0.60)", bits,
                      with, without, ratio);
        check_true(label, with > 0 && ratio < 0.6);
    }
}

// ============================================================================
// Round-trip at bit depths a WAV container cannot express
// ============================================================================

// Pack `value` (right-justified, `bits` significant) into PcmFormat's
// left-justified little-endian layout, filling the unused low bits with
// `junk` to prove the encoder ignores them.
void pack_sample(uint8_t* dst, int32_t value, uint32_t bits, uint32_t bytes, uint32_t junk) {
    const uint32_t shift = bytes * 8 - bits;
    const uint32_t low_mask = (1U << shift) - 1U;
    const uint32_t u = (static_cast<uint32_t>(value) << shift) | (junk & low_mask);
    for (uint32_t b = 0; b < bytes; b++) {
        dst[b] = static_cast<uint8_t>(u >> (8 * b));
    }
}

// Deterministic test signal for `n` frames of `channels` channels at `bits`,
// spanning the full signed range of that depth including both extremes, and
// mixing a compressible ramp with an incompressible pseudorandom component so
// both FIXED and VERBATIM subframes get exercised. Returned packed, plus the
// same bytes with the unused low bits cleared: what the decoder must return.
void make_signal(uint32_t n, uint32_t channels, uint32_t bits, std::vector<uint8_t>& packed,
                 std::vector<uint8_t>& expected) {
    const int32_t peak = static_cast<int32_t>((1U << (bits - 1)) - 1U);
    const int32_t floor_value = -peak - 1;
    const uint32_t bytes = (bits + 7) / 8;
    packed.assign(static_cast<size_t>(n) * channels * bytes, 0);
    expected.assign(packed.size(), 0);

    uint32_t state = 0x12345678U;
    size_t pos = 0;
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t c = 0; c < channels; c++) {
            // xorshift32, so the sequence is identical on every platform
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;

            int32_t v = 0;
            if (i >= n / 2 && i < (n / 2) + 32) {
                // A stretch of full-scale alternation on every channel: the
                // largest residuals any predictor order can produce, which is
                // what Pass A's depth-dependent spill interval and the side
                // channel's extra bit exist for.
                v = ((i + c) % 2 == 0) ? peak : floor_value;
            } else if (i % 64 == 0) {
                v = (((i / 64) + c) % 2 == 0) ? peak : floor_value;  // both extremes, every channel
            } else if (i % 3 == 0) {
                v = static_cast<int32_t>(state % static_cast<uint32_t>(peak)) - (peak / 2);
            } else {
                v = static_cast<int32_t>((i * (c + 3)) % static_cast<uint32_t>(peak)) - (peak / 2);
            }
            v = v < floor_value ? floor_value : (v > peak ? peak : v);
            pack_sample(&packed[pos], v, bits, bytes, state >> 8);
            pack_sample(&expected[pos], v, bits, bytes, 0);
            pos += bytes;
        }
    }
}

// Encode `packed` as one whole stream the way a file writer would: the
// header, the documented encode() loop, finish() with the remainder, then the
// finished header written over the first. Appends the stream to `stream`,
// and each frame's channel-assignment code (frame byte 3's upper nibble) to
// `modes` if given. Returns false, after printing why, on any unexpected
// result.
bool encode_stream(FLACEncoder& encoder, const uint8_t* packed, size_t packed_size,
                   std::vector<uint8_t>& stream, const char* label,
                   std::vector<uint8_t>* modes = nullptr) {
    std::vector<uint8_t> frame(encoder.get_max_output_bytes());
    size_t written = 0;
    size_t consumed = 0;
    const size_t start = stream.size();

    if (encoder.write_header(frame.data(), frame.size(), written) != FLAC_ENCODER_SUCCESS) {
        std::printf("  %-44s write_header() failed\n", label);
        return false;
    }
    stream.insert(stream.end(), frame.begin(), frame.begin() + static_cast<long>(written));

    const uint8_t* p = packed;
    size_t remaining = packed_size;
    FLACEncoderResult r = FLAC_ENCODER_SUCCESS;
    while ((r = encoder.encode(p, remaining, frame.data(), frame.size(), consumed, written)) ==
           FLAC_ENCODER_SUCCESS) {
        stream.insert(stream.end(), frame.begin(), frame.begin() + static_cast<long>(written));
        if (modes != nullptr) {
            modes->push_back(static_cast<uint8_t>(frame[3] >> 4));
        }
        p += consumed;
        remaining -= consumed;
    }
    if (r != FLAC_ENCODER_NEED_MORE_DATA) {
        std::printf("  %-44s encode() returned %d\n", label, static_cast<int>(r));
        return false;
    }
    r = encoder.finish(p, remaining, frame.data(), frame.size(), written);
    if (r != FLAC_ENCODER_SUCCESS) {
        std::printf("  %-44s finish() returned %d\n", label, static_cast<int>(r));
        return false;
    }
    stream.insert(stream.end(), frame.begin(), frame.begin() + static_cast<long>(written));

    if (encoder.write_header(stream.data() + start, FLACEncoder::HEADER_BYTES, written) !=
        FLAC_ENCODER_SUCCESS) {
        std::printf("  %-44s finished write_header() failed\n", label);
        return false;
    }
    return true;
}

bool encode_stream(FLACEncoder& encoder, const std::vector<uint8_t>& packed,
                   std::vector<uint8_t>& stream, const char* label,
                   std::vector<uint8_t>* modes = nullptr) {
    return encode_stream(encoder, packed.data(), packed.size(), stream, label, modes);
}

// Decode a whole stream through the decoder's byte output into `decoded`,
// reporting the sample rate and total sample count its STREAMINFO carries.
bool decode_stream(const std::vector<uint8_t>& stream, std::vector<uint8_t>& decoded,
                   uint32_t& sample_rate, uint64_t& total_samples, const char* label) {
    FLACDecoder decoder;
    std::vector<uint8_t> out_block;
    const uint8_t* in = stream.data();
    size_t left = stream.size();

    while (left > 0) {
        size_t used = 0;
        size_t produced = 0;
        const FLACDecoderResult d =
            decoder.decode(in, left, out_block.data(), out_block.size(), used, produced);
        in += used;
        left -= used;

        if (d == FLAC_DECODER_HEADER_READY) {
            const FLACStreamInfo& info = decoder.get_stream_info();
            sample_rate = info.sample_rate();
            total_samples = info.total_samples_per_channel();
            out_block.resize(static_cast<size_t>(decoder.get_output_buffer_size_samples()) *
                             info.bytes_per_sample());
        } else if (d == FLAC_DECODER_SUCCESS) {
            const size_t bytes = produced * decoder.get_stream_info().bytes_per_sample();
            decoded.insert(decoded.end(), out_block.begin(),
                           out_block.begin() + static_cast<long>(bytes));
        } else if (d < 0) {
            std::printf("  %-44s decode() returned %d\n", label, static_cast<int>(d));
            return false;
        } else if (d == FLAC_DECODER_END_OF_STREAM || used == 0) {
            break;  // The end, or NEED_MORE_DATA with nothing left to give
        }
    }
    return true;
}

// Encode `packed` and decode it back, requiring the decoder's byte output to
// equal `expected` and its STREAMINFO to report `frames` samples at `rate`.
bool encode_decode_matches(FLACEncoder& encoder, const std::vector<uint8_t>& packed,
                           const std::vector<uint8_t>& expected, uint64_t frames, const char* label,
                           std::vector<uint8_t>* modes = nullptr) {
    std::vector<uint8_t> stream;
    if (!encode_stream(encoder, packed, stream, label, modes)) {
        return false;
    }
    std::vector<uint8_t> decoded;
    uint32_t rate = 0;
    uint64_t total = 0;
    if (!decode_stream(stream, decoded, rate, total, label)) {
        return false;
    }
    if (rate != encoder.get_pcm_format().sample_rate() || total != frames) {
        std::printf("  %-44s STREAMINFO says %u Hz / %llu samples, expected %u Hz / %llu\n", label,
                    rate, static_cast<unsigned long long>(total),
                    encoder.get_pcm_format().sample_rate(),
                    static_cast<unsigned long long>(frames));
        return false;
    }
    if (decoded != expected) {
        std::printf("  %-44s decoded bytes differ (%zu vs %zu bytes)\n", label, decoded.size(),
                    expected.size());
        return false;
    }
    return true;
}

// Round-trip a make_signal() signal at `bits`/`channels`: three full blocks
// plus a 7-sample tail, which exercises finish()'s short final frame (the
// only frame allowed under 16 samples).
bool round_trip(uint32_t bits, uint32_t channels, uint32_t block_size, const char* label) {
    const uint32_t total_frames = block_size * 3 + 7;
    std::vector<uint8_t> packed;
    std::vector<uint8_t> expected;
    make_signal(total_frames, channels, bits, packed, expected);
    FLACEncoder encoder(make_format(channels, bits), make_options(block_size));
    return encode_decode_matches(encoder, packed, expected, total_frames, label);
}

// ============================================================================
// Stereo channel assignments
// ============================================================================

enum class StereoCase : uint8_t { LEFT_SIDE, RIGHT_SIDE, MID_SIDE, INDEPENDENT };

// Stereo input at `bits` built so one channel assignment is clearly the
// cheapest: a slow sine (nearly free under a fixed predictor) combined with
// independent noise. Left/side wants L clean and R = L + noise; right/side
// the mirror image; mid/side wants R = -L, which makes mid constant; and
// independent wants L clean and R uncorrelated noise. Packed per
// pcm_format.h with the unused low bits clear, so it is also the expected
// decoder output.
std::vector<uint8_t> make_stereo(uint32_t n, uint32_t bits, StereoCase which) {
    const uint32_t bytes = (bits + 7) / 8;
    const double amp = static_cast<double>(1U << (bits - 3));
    const uint32_t noise_span = 1U << (bits - 4);
    std::vector<uint8_t> out(static_cast<size_t>(n) * 2 * bytes);
    uint32_t state = 0x9E3779B9U;
    const auto noise = [&]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<int32_t>(state % noise_span) - static_cast<int32_t>(noise_span / 2);
    };
    for (uint32_t i = 0; i < n; i++) {
        const int32_t sine =
            static_cast<int32_t>(amp * std::sin(2.0 * 3.14159265358979 * i / 997.0));
        int32_t l = sine;
        int32_t r = sine;
        switch (which) {
            case StereoCase::LEFT_SIDE:
                r = sine + noise();
                break;
            case StereoCase::RIGHT_SIDE:
                l = sine + noise();
                break;
            case StereoCase::MID_SIDE:
                l = sine + noise();
                r = -l;
                break;
            case StereoCase::INDEPENDENT:
                r = noise() * 4;
                break;
        }
        pack_sample(&out[static_cast<size_t>(i) * 2 * bytes], l, bits, bytes, 0);
        pack_sample(&out[(static_cast<size_t>(i) * 2 * bytes) + bytes], r, bits, bytes, 0);
    }
    return out;
}

void check_stereo_modes() {
    std::printf("\nEvery stereo channel assignment is chosen and round-trips:\n");

    // 8, 12/16 and 24 bits are the 1-, 2- and 3-byte unpacking, whose Pass B
    // derives each assignment's signals from the packed input chunk by chunk.
    struct Case {
        StereoCase which;
        uint8_t code;
        const char* name;
    };
    static const Case CASES[] = {
        {StereoCase::LEFT_SIDE, 8, "left/side"},
        {StereoCase::RIGHT_SIDE, 9, "right/side"},
        {StereoCase::MID_SIDE, 10, "mid/side"},
        {StereoCase::INDEPENDENT, 1, "independent"},
    };
    const uint32_t frames = BLOCK * 3 + 7;
    for (const uint32_t bits : {8U, 12U, 16U, 24U}) {
        for (const Case& c : CASES) {
            char label[96];
            std::snprintf(label, sizeof(label), "%u-bit %s", bits, c.name);
            const std::vector<uint8_t> pcm = make_stereo(frames, bits, c.which);
            FLACEncoder e(make_format(2, bits), make_options(BLOCK));
            std::vector<uint8_t> modes;
            const bool ok = encode_decode_matches(e, pcm, pcm, frames, label, &modes);
            bool all_chosen = !modes.empty();
            for (const uint8_t m : modes) {
                all_chosen = all_chosen && (m == c.code);
            }
            check_true(label, ok && all_chosen);
        }
    }
}

// ============================================================================
// Stream boundaries and repeatability
// ============================================================================

void check_stream_boundaries() {
    std::printf("\nStream boundaries and repeatability:\n");

    // Input that ends exactly on a block boundary: finish() gets nothing,
    // writes nothing, and the finished header still counts every sample.
    {
        const uint32_t frames = BLOCK * 3;
        std::vector<uint8_t> packed;
        std::vector<uint8_t> expected;
        make_signal(frames, 2, 16, packed, expected);
        FLACEncoder e(make_format(2, 16), make_options(BLOCK));
        check_true("input an exact multiple of the block size",
                   encode_decode_matches(e, packed, expected, frames,
                                         "input an exact multiple of the block size"));
        std::vector<uint8_t> out(e.get_max_output_bytes());
        size_t consumed = 0;
        size_t written = 0;
        check("  encode() afterwards",
              e.encode(packed.data(), packed.size(), out.data(), out.size(), consumed, written),
              FLAC_ENCODER_ERROR_STREAM_FINISHED);
    }

    // reset() starts an independent stream: the same input must produce a
    // byte-identical stream (frame numbers restarting at 0, and a finished
    // header with only the second stream's frame sizes).
    for (const uint32_t bits : {16U, 24U}) {
        std::vector<uint8_t> packed;
        std::vector<uint8_t> expected;
        make_signal(BLOCK * 2 + 100, 2, bits, packed, expected);
        FLACEncoder e(make_format(2, bits), make_options(BLOCK));
        std::vector<uint8_t> first;
        std::vector<uint8_t> second;
        const bool ok1 = encode_stream(e, packed, first, "reset() repeatability");
        e.reset();
        const bool ok2 = encode_stream(e, packed, second, "reset() repeatability");
        char label[96];
        std::snprintf(label, sizeof(label), "%u-bit stream after reset() is byte-identical", bits);
        check_true(label, ok1 && ok2 && first == second);
    }
}

// ============================================================================
// Reconfiguration
// ============================================================================

// Whether `e` holds exactly the state a fresh FLACEncoder(format, options)
// would: everything derived from the configuration, and a new stream.
bool matches_fresh(const FLACEncoder& e, const PcmFormat& format,
                   const FLACEncoderOptions& options) {
    const FLACEncoder fresh(format, options);
    const bool same =
        e.config_result_ == fresh.config_result_ &&
        e.max_output_bytes_ == fresh.max_output_bytes_ &&
        e.input_block_bytes_ == fresh.input_block_bytes_ &&
        e.bits_per_sample_code_ == fresh.bits_per_sample_code_ &&
        e.sample_rate_code_ == fresh.sample_rate_code_ &&
        e.sample_rate_extra_[0] == fresh.sample_rate_extra_[0] &&
        e.sample_rate_extra_[1] == fresh.sample_rate_extra_[1] &&
        e.sample_rate_extra_len_ == fresh.sample_rate_extra_len_ &&
        e.total_samples_ == fresh.total_samples_ && e.frame_number_ == fresh.frame_number_ &&
        e.min_frame_bytes_ == fresh.min_frame_bytes_ &&
        e.max_frame_bytes_ == fresh.max_frame_bytes_ && e.finished_ == fresh.finished_ &&
        e.get_pcm_format().sample_rate() == format.sample_rate() &&
        e.get_pcm_format().num_channels() == format.num_channels() &&
        e.get_pcm_format().bits_per_sample() == format.bits_per_sample() &&
        e.get_options().block_size == options.block_size &&
        e.get_options().wasted_bits == options.wasted_bits;
    return same;
}

// Whether `e` encodes `packed` to the same bytes a fresh
// FLACEncoder(format, options) does
bool encodes_like_fresh(FLACEncoder& e, const PcmFormat& format, const FLACEncoderOptions& options,
                        const std::vector<uint8_t>& packed, const char* label) {
    FLACEncoder fresh(format, options);
    std::vector<uint8_t> got;
    std::vector<uint8_t> want;
    const bool ok1 = encode_stream(e, packed, got, label);
    const bool ok2 = encode_stream(fresh, packed, want, label);
    return ok1 && ok2 && got == want;
}

void check_reconfigure() {
    std::printf("\nreset(format, options) leaves the encoder as the constructor would:\n");

    // Rich: a sample rate carried in two extra header bytes (code 13), so every
    // derived field is set
    const PcmFormat rich = make_format(2, 16, 22051);
    FLACEncoderOptions rich_options = make_options(1152);
    // Plain: mono 24-bit at a table rate with every option off, wasted-bits
    // detection included, so stale options would show
    const PcmFormat plain = make_format(1, 24, 48000);
    FLACEncoderOptions plain_options = make_options(BLOCK);
    plain_options.wasted_bits = false;
    const PcmFormat bad = make_format(9, 16);

    std::vector<uint8_t> rich_input;
    std::vector<uint8_t> plain_input;
    std::vector<uint8_t> expected;
    make_signal(1152 * 3 + 100, 2, 16, rich_input, expected);
    make_signal(BLOCK * 2 + 100, 1, 24, plain_input, expected);

    FLACEncoder e(rich, rich_options);
    // Leave a stream in progress: one frame written, not finished
    {
        std::vector<uint8_t> out(e.get_max_output_bytes());
        size_t consumed = 0;
        size_t written = 0;
        check("rich: encode() one frame",
              e.encode(rich_input.data(), rich_input.size(), out.data(), out.size(), consumed,
                       written),
              FLAC_ENCODER_SUCCESS);
    }

    // A rejected configuration fails before deriving anything, so whatever
    // the rich one derived must already be cleared
    e.reset(bad, rich_options);
    check_true("rich mid-stream -> unsupported: state matches a fresh encoder",
               matches_fresh(e, bad, rich_options));
    check("  get_max_output_bytes()", static_cast<long long>(e.get_max_output_bytes()), 0);
    {
        std::vector<uint8_t> out(64);
        size_t written = 0;
        check("  write_header()", e.write_header(out.data(), out.size(), written),
              FLAC_ENCODER_ERROR_BAD_CONFIG);
    }

    e.reset(rich, rich_options);
    check_true("unsupported -> rich: state matches a fresh encoder",
               matches_fresh(e, rich, rich_options));
    check_true("  and encodes byte-identically",
               encodes_like_fresh(e, rich, rich_options, rich_input, "unsupported -> rich"));

    e.reset(plain, plain_options);
    check_true("rich finished -> plain: state matches a fresh encoder",
               matches_fresh(e, plain, plain_options));
    check_true("  and encodes byte-identically",
               encodes_like_fresh(e, plain, plain_options, plain_input, "rich -> plain"));

    e.reset(rich, rich_options);
    // Passing the encoder its own configuration is the same as reset()
    e.reset(e.get_pcm_format(), e.get_options());
    check_true("own configuration (aliased arguments): state matches a fresh encoder",
               matches_fresh(e, rich, rich_options));
    check_true("  and encodes byte-identically",
               encodes_like_fresh(e, rich, rich_options, rich_input, "own configuration"));
}

// ============================================================================
// Sample rates outside the frame header's table
// ============================================================================

void check_sample_rate_codes() {
    std::printf("\nSample rates outside the table round-trip, with the code that carries them:\n");

    // Frame header byte 2's low nibble (RFC 9639 SS9.1.2): 0 = from
    // STREAMINFO, 12 = kHz in one extra byte, 13 = Hz in two, 14 = tens of Hz
    // in two.
    struct Case {
        uint32_t rate;
        uint8_t code;
    };
    static const Case CASES[] = {
        {12000, 12},  {50000, 12}, {255000, 12}, {44101, 13},  {65535, 13},
        {256000, 14}, {96010, 14}, {655350, 14}, {65536, 0},   {65537, 0},
        {88201, 0},   {655351, 0}, {655360, 0},  {1048575, 0},
    };
    for (const Case& c : CASES) {
        const uint32_t frames = 1000 * 2 + 5;
        std::vector<uint8_t> packed;
        std::vector<uint8_t> expected;
        make_signal(frames, 1, 16, packed, expected);
        const PcmFormat format = make_format(1, 16, c.rate);
        FLACEncoder e(format, make_options(1000));
        std::vector<uint8_t> stream;
        char label[96];
        std::snprintf(label, sizeof(label), "%u Hz (code %u)", c.rate, c.code);
        const bool ok = encode_decode_matches(e, packed, expected, frames, label);
        FLACEncoder e2(format, make_options(1000));
        const bool enc = encode_stream(e2, packed, stream, label);
        const uint8_t code =
            enc ? static_cast<uint8_t>(stream[FLACEncoder::HEADER_BYTES + 2] & 0x0FU) : 0;
        // STREAMINFO's 20-bit rate (bytes 10-12 of its body), which code 0
        // relies on: the decoder reads back the field the encoder wrote, so
        // check its bits directly
        uint32_t si_rate = 0;
        if (enc) {
            const uint8_t* si = &stream[8];
            si_rate = (static_cast<uint32_t>(si[10]) << 12) | (static_cast<uint32_t>(si[11]) << 4) |
                      (static_cast<uint32_t>(si[12]) >> 4);
        }
        check_true(label, ok && code == c.code && si_rate == c.rate);
    }
}

void check_odd_depth_round_trips() {
    std::printf("\nRound-trip at depths WAV cannot carry (decoder bytes == encoder bytes):\n");

    // Every depth from the format minimum to this encoder's maximum, at the
    // channel counts that select different internal paths: 1 is mono, 2
    // exercises the joint-stereo assignments, and 5 forces the independent
    // multichannel path.
    const int failures_before = failures;
    for (uint32_t bits = 4; bits <= 24; bits++) {
        for (const uint32_t channels : {1U, 2U, 5U}) {
            char label[96];
            std::snprintf(label, sizeof(label), "%u-bit, %u channel%s", bits, channels,
                          channels == 1 ? "" : "s");
            if (!round_trip(bits, channels, 512, label)) {
                failures++;
                std::printf("  %-44s <-- FAIL\n", label);
            }
        }
    }
    std::printf("  depths 4-24 x {1, 2, 5} channels: %s\n",
                failures == failures_before ? "all bit-exact" : "see failures above");

    // Large blocks, past every internal chunk size (Pass A's spill interval,
    // Pass B's residual chunk): short and long spill intervals, the side
    // channel's extra bit, and the multichannel path.
    const int deep_failures_before = failures;
    constexpr uint32_t DEEP_BLOCK = 16384;
    for (const uint32_t bits : {8U, 16U, 17U, 24U}) {
        for (const uint32_t channels : {1U, 2U, 5U}) {
            char label[96];
            std::snprintf(label, sizeof(label), "%u-bit, %u channel%s, %u-sample blocks", bits,
                          channels, channels == 1 ? "" : "s", DEEP_BLOCK);
            if (!round_trip(bits, channels, DEEP_BLOCK, label)) {
                failures++;
                std::printf("  %-44s <-- FAIL\n", label);
            }
        }
    }
    std::printf("  depths {8, 16, 17, 24} x {1, 2, 5} channels at %u-sample blocks: %s\n",
                DEEP_BLOCK,
                failures == deep_failures_before ? "all bit-exact" : "see failures above");

    // The encoder unpacks CHUNK_SAMPLES per channel at a time, carrying
    // Pass A's scan state and Pass B's difference cascade across chunks.
    // Block sizes on and either side of chunk multiples put every chunk
    // boundary case in play: a block that is exactly one chunk, one sample
    // past it (a 1-sample last chunk), and one short of two; round_trip()'s
    // 7-sample tail adds a final frame far under one chunk. The odd block
    // sizes also start some blocks at an address that is not 4-byte aligned
    // (every block after the first for 24-bit mono, alternate blocks for
    // 24-bit stereo), which sends 24-bit input down the byte-load fallback
    // of the word-at-a-time unpacking; aligned starts take the word path.
    const int chunk_failures_before = failures;
    const uint32_t chunk = FLACEncoder::CHUNK_SAMPLES;
    for (const uint32_t block : {chunk - 1, chunk, chunk + 1, (2 * chunk) - 1, (2 * chunk) + 1}) {
        for (const uint32_t bits : {12U, 16U, 24U}) {
            for (const uint32_t channels : {1U, 2U, 5U}) {
                char label[96];
                std::snprintf(label, sizeof(label), "%u-bit, %u channel%s, %u-sample blocks", bits,
                              channels, channels == 1 ? "" : "s", block);
                if (!round_trip(bits, channels, block, label)) {
                    failures++;
                    std::printf("  %-44s <-- FAIL\n", label);
                }
            }
        }
    }
    std::printf("  {12, 16, 24}-bit x {1, 2, 5} channels at blocks around %u-sample chunks: %s\n",
                chunk, failures == chunk_failures_before ? "all bit-exact" : "see failures above");
}

// Full-scale alternation on every sample: the input that drives Pass A's
// order-4 partial sums to their proven maximum, 2^32 - 8 * interval at the
// deepest subframes (sum_chunk_samples() in flac_encoder.cpp). Stereo runs in
// phase (side all zero) and in antiphase (side at +/-(2^bits - 1), a full
// (bits + 1)-bit swing). An overflowed sum would not change the encoded
// bytes, so the round trip alone cannot catch one; this build defines
// MICRO_FLAC_CHECK_SCAN_SUMS (CMakeLists.txt), which makes the encoder trap
// on any partial past 32 bits, and this is the input that reaches the limit.
void check_scan_worst_case() {
    std::printf("\nPass A spill interval at full-scale alternation\n");
    const int failures_before = failures;
    const uint32_t total_frames = BLOCK * 2 + 7;
    for (const uint32_t bits : {16U, 20U, 22U, 23U, 24U}) {
        const int32_t peak = static_cast<int32_t>((1U << (bits - 1)) - 1U);
        const int32_t floor_value = -peak - 1;
        const uint32_t bytes = (bits + 7) / 8;
        for (const uint32_t channels : {1U, 2U, 3U}) {
            for (const bool antiphase : {false, true}) {
                if (channels == 1 && antiphase) {
                    continue;
                }
                std::vector<uint8_t> packed(static_cast<size_t>(total_frames) * channels * bytes);
                size_t pos = 0;
                for (uint32_t i = 0; i < total_frames; i++) {
                    for (uint32_t c = 0; c < channels; c++) {
                        const bool high = ((i + (antiphase ? c : 0)) % 2) == 0;
                        pack_sample(&packed[pos], high ? peak : floor_value, bits, bytes, 0);
                        pos += bytes;
                    }
                }
                char label[96];
                std::snprintf(label, sizeof(label), "%u-bit %u ch %s", bits, channels,
                              antiphase ? "antiphase" : "in phase");
                FLACEncoder encoder(make_format(channels, bits), make_options(BLOCK));
                if (!encode_decode_matches(encoder, packed, packed, total_frames, label)) {
                    failures++;
                    std::printf("  %-44s <-- FAIL\n", label);
                }
            }
        }
    }
    std::printf("  {16, 20, 22, 23, 24}-bit x {1, 2, 3} channels: %s\n",
                failures == failures_before ? "all bit-exact" : "see failures above");
}

// Encode `packed` whole and return the stream's size, or 0 on failure.
size_t encoded_size(const PcmFormat& format, const FLACEncoderOptions& options,
                    const std::vector<uint8_t>& packed,
                    std::vector<uint8_t>* stream_out = nullptr) {
    FLACEncoder encoder(format, options);
    std::vector<uint8_t> stream;
    if (!encode_stream(encoder, packed, stream, "encoded_size")) {
        return 0;
    }
    if (stream_out != nullptr) {
        *stream_out = stream;
    }
    return stream.size();
}

// ============================================================================
// Wasted bits
// ============================================================================

// Tonal content with a little noise, channel c at depth bits - wasted[c] and
// shifted up by wasted[c], so every sample of channel c ends in wasted[c] zero
// bits. A channel with wasted[c] == bits is digital silence. Packed with the
// unused low bits clear, so it is also the expected decoder output.
std::vector<uint8_t> make_wasted(uint32_t n, uint32_t channels, uint32_t bits,
                                 const uint32_t* wasted) {
    const uint32_t bytes = (bits + 7) / 8;
    std::vector<uint8_t> out(static_cast<size_t>(n) * channels * bytes);
    uint32_t seed = 12345;
    for (uint32_t i = 0; i < n; i++) {
        const double t = static_cast<double>(i);
        for (uint32_t c = 0; c < channels; c++) {
            const uint32_t k = wasted[c];
            int32_t v = 0;
            if (k < bits) {
                const uint32_t depth = bits - k;
                const int32_t hi = static_cast<int32_t>((1U << (depth - 1)) - 1U);
                const double amp = 0.4 * static_cast<double>(1U << (depth - 1));
                seed = (seed * 1664525U) + 1013904223U;
                const uint32_t spread = 1U << (depth / 3);
                const int32_t noise =
                    static_cast<int32_t>(seed % ((2 * spread) + 1)) - static_cast<int32_t>(spread);
                v = static_cast<int32_t>(
                        amp * ((0.6 * std::sin(t / (37.1 + c))) + (0.4 * std::sin(t / 5.3)))) +
                    noise;
                v = (v > hi) ? hi : ((v < -hi - 1) ? -hi - 1 : v);
                v = static_cast<int32_t>(static_cast<uint32_t>(v) << k);
            }
            pack_sample(&out[(static_cast<size_t>(i) * channels + c) * bytes], v, bits, bytes, 0);
        }
    }
    return out;
}

void check_wasted_bits() {
    std::printf("\nWasted bits (FLACEncoderOptions::wasted_bits):\n");

    // Round trips across formats, with every channel sharing 1, 4 or 8
    // wasted bits, with a different count per channel (so mid and side get
    // their own), and with one channel silent. The option on and off; and
    // forced independent stereo, whose Pass A finds left's and right's
    // wasted bits without deriving mid and side.
    struct Format {
        uint32_t channels;
        uint32_t bits;
    };
    const Format formats[] = {{1, 24}, {2, 24}, {2, 20}, {2, 17}, {1, 17},
                              {2, 12}, {2, 8},  {5, 24}, {2, 16}, {1, 16}};
    const uint32_t mixed[8] = {8, 3, 0, 5, 1, 7, 2, 6};
    const int failures_before = failures;
    uint32_t cases = 0;
    for (const Format& f : formats) {
        for (uint32_t shape = 0; shape < 5; shape++) {
            uint32_t wasted[8];
            for (uint32_t c = 0; c < 8; c++) {
                const uint32_t all[3] = {1, 4, 8};
                uint32_t k = (shape < 3) ? all[shape] : mixed[c];
                if (shape == 4) {
                    k = (c == 0) ? f.bits : 4;  // Channel 0 silent
                }
                wasted[c] = (k >= f.bits && shape != 4) ? f.bits - 2 : k;
            }
            for (const uint32_t block : {1152U, 4096U}) {
                const uint32_t n = (2 * block) + 7;
                const std::vector<uint8_t> input = make_wasted(n, f.channels, f.bits, wasted);
                for (const uint32_t opt : {0U, 4U, 5U}) {
                    FLACEncoderOptions options = make_options(block);
                    options.wasted_bits = (opt != 4);
                    char label[96];
                    std::snprintf(label, sizeof(label), "%uch %u-bit shape %u block %u opt %u",
                                  f.channels, f.bits, shape, block, opt);
                    FLACEncoder encoder(make_format(f.channels, f.bits), options);
                    encoder.stereo_estimation_enabled_ = (opt != 5);
                    if (!encode_decode_matches(encoder, input, input, n, label)) {
                        failures++;
                        std::printf("  %-44s <-- FAIL\n", label);
                    }
                    cases++;
                }
            }
        }
    }
    std::printf(
        "  %u round trips (10 formats x 5 wasted-bit shapes x 2 block sizes x 3 options): %s\n",
        cases, failures == failures_before ? "all bit-exact" : "see failures above");

    // 16-bit audio finds its wasted bits like every other depth. 8-bit audio
    // carried in 16 bits codes as the 8-bit stream does, plus each frame's
    // unary count: 8 bits per subframe in mono, and in stereo within one more
    // bit per sample (mid keeps only k - 1, see encode_frame()).
    {
        const uint32_t no_shift[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        const uint32_t shift8[8] = {8, 8, 8, 8, 8, 8, 8, 8};
        const uint32_t frames = (3 * 4096) + 7;
        const size_t blocks = (frames + 4095) / 4096;
        for (const uint32_t channels : {1U, 2U}) {
            const FLACEncoderOptions options = make_options(4096);
            const std::vector<uint8_t> in16 = make_wasted(frames, channels, 16, shift8);
            const size_t size_on = encoded_size(make_format(channels, 16), options, in16);
            const size_t size8 = encoded_size(make_format(channels, 8), options,
                                              make_wasted(frames, channels, 8, no_shift));
            char what[96];
            std::snprintf(what, sizeof(what), "  8-bit %uch in 16 bits: bytes over 8-bit",
                          channels);
            if (channels == 1) {
                check(what, static_cast<long long>(size_on) - static_cast<long long>(size8),
                      static_cast<long long>(blocks));
            } else {
                check_true(what, size_on <= size8 + (frames / 8) + (2 * blocks));
            }
        }
    }

    // 16-bit mono carried in 24 bits codes exactly as 16-bit mono does, but
    // for the 8 unary bits each subframe header spends on the count: the
    // analysis of the shifted samples is the 16-bit stream's.
    const uint32_t none[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    const uint32_t eight[8] = {8, 8, 8, 8, 8, 8, 8, 8};
    const uint32_t n = (3 * 4096) + 7;
    {
        const FLACEncoderOptions options = make_options(4096);
        const size_t size16 =
            encoded_size(make_format(1, 16), options, make_wasted(n, 1, 16, none));
        const size_t size24 =
            encoded_size(make_format(1, 24), options, make_wasted(n, 1, 24, eight));
        check("16-bit mono in 24 bits: bytes over 16-bit",
              static_cast<long long>(size24) - static_cast<long long>(size16), 4);
    }

    // Stereo 16-bit audio in 24 bits: within the 16-bit stream plus one bit
    // per sample and each frame's two unary counts (mid is coded as L + R,
    // one bit wider, see encode_frame(); libFLAC pays the same), and far
    // below coding it with the option off.
    {
        const std::vector<uint8_t> in24 = make_wasted(n, 2, 24, eight);
        FLACEncoderOptions options = make_options(4096);
        const size_t size16 =
            encoded_size(make_format(2, 16), options, make_wasted(n, 2, 16, none));
        const size_t size_on = encoded_size(make_format(2, 24), options, in24);
        options.wasted_bits = false;
        const size_t size_off = encoded_size(make_format(2, 24), options, in24);
        std::printf("  16-bit stereo in 24 bits: %zu bytes, 16-bit stream %zu, option off %zu\n",
                    size_on, size16, size_off);
        const size_t frames = (n + 4095) / 4096;
        check_true("  within 16-bit + 1 bit per sample + 2 bytes per frame",
                   size_on <= size16 + (n / 8) + (2 * frames));
        check_true("  under 3/4 of the option off", size_on * 4 < size_off * 3);
    }

    // Without wasted bits the option changes nothing, byte for byte.
    int identical_failures = 0;
    for (const Format& f : formats) {
        FLACEncoderOptions options = make_options(4096);
        const std::vector<uint8_t> input = make_wasted(n, f.channels, f.bits, none);
        std::vector<uint8_t> on;
        std::vector<uint8_t> off;
        encoded_size(make_format(f.channels, f.bits), options, input, &on);
        options.wasted_bits = false;
        encoded_size(make_format(f.channels, f.bits), options, input, &off);
        if (on != off || on.empty()) {
            identical_failures++;
            std::printf("  %uch %u-bit: output differs with the option on\n", f.channels, f.bits);
        }
    }
    check("  formats whose output changes without wasted bits", identical_failures, 0);
}

// 3-byte samples at a misaligned address, ending exactly where their heap
// allocation does: on hosts that unpack a 24-bit sample with one unaligned
// 4-byte load (FLAC_FAST_UNALIGNED_LOADS), no load may read past the
// caller's input, which ASan would report here. The misalignment keeps mono
// and stereo off their aligned word paths, the odd block sizes put partial
// chunks everywhere, and finish() gets a short final block that ends the
// buffer too.
void check_unaligned_input_end() {
    std::printf("\n3-byte input misaligned and ending at its allocation's end:\n");
    const int failures_before = failures;
    for (const uint32_t bits : {17U, 24U}) {
        for (const uint32_t channels : {1U, 2U, 3U, 8U}) {
            for (const uint32_t block : {16U, 17U, 257U}) {
                const uint32_t frames = (block * 2) + 5;
                std::vector<uint8_t> packed;
                std::vector<uint8_t> expected;
                make_signal(frames, channels, bits, packed, expected);
                // One byte of lead-in makes the start odd; the input's
                // last byte is the allocation's last.
                std::unique_ptr<uint8_t[]> buffer(new uint8_t[packed.size() + 1]);
                std::memcpy(buffer.get() + 1, packed.data(), packed.size());
                char label[96];
                std::snprintf(label, sizeof(label), "%u-bit %u ch block %u", bits, channels, block);
                FLACEncoder encoder(make_format(channels, bits), make_options(block));
                std::vector<uint8_t> stream;
                std::vector<uint8_t> decoded;
                uint32_t rate = 0;
                uint64_t total = 0;
                const bool ok =
                    encode_stream(encoder, buffer.get() + 1, packed.size(), stream, label) &&
                    decode_stream(stream, decoded, rate, total, label) && total == frames &&
                    decoded == expected;
                if (!ok) {
                    failures++;
                    std::printf("  %-44s <-- FAIL\n", label);
                }
            }
        }
    }
    std::printf("  {17, 24}-bit x {1, 2, 3, 8} ch x blocks {16, 17, 257}: %s\n",
                failures == failures_before ? "all bit-exact" : "see failures above");
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
    std::printf("test_encoder_config: FLACEncoder API contract and validation\n\n");

    check_config_validation();
    check_no_allocation();
    check_encode_arguments();
    check_finish_and_lifecycle();
    check_header();
    check_stereo_estimation();
    check_stereo_modes();
    check_stream_boundaries();
    check_reconfigure();
    check_sample_rate_codes();
    check_odd_depth_round_trips();
    check_unaligned_input_end();
    check_scan_worst_case();
    check_wasted_bits();

    if (failures != 0) {
        std::printf("\ntest_encoder_config: %d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\ntest_encoder_config: all tests passed\n");
    return 0;
}

// NOLINTEND(readability-magic-numbers)
