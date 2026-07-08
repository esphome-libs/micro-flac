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

// Regression tests for streaming error recovery and frame-header validation.
//
// Builds a synthetic native FLAC stream (mono, 16-bit, fixed block size 192,
// constant subframes) so every byte offset and CRC is known exactly, then:
//
//   1. Baseline decode (one-shot and chunked) must produce every frame.
//   2. A corrupted frame body must surface exactly one CRC_MISMATCH whose
//      bytes_consumed lands exactly at the next frame's first byte, so a
//      caller following the documented recovery contract (advance by
//      bytes_consumed, retry) loses only the corrupted frame. Regression
//      test for the missing buffer_index_ re-basing on decode_frame's
//      subframe/footer error paths.
//   3. Frames with a malformed coded frame number (invalid 0xFF leading
//      byte, invalid continuation byte, or the 7-byte sample-number form
//      inside a fixed-block-size stream) must be rejected even though their
//      CRC-8/CRC-16 are internally consistent.
//
// Exits 0 on success; prints each failure and exits 1 otherwise.

#include "micro_flac/flac_decoder.h"

#include "crc.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using micro_flac::calculate_crc8;
using micro_flac::FLACDecoder;
using micro_flac::FLACDecoderResult;
using micro_flac::update_crc16;

namespace {

int g_failures = 0;

#define EXPECT(cond, ...)                                             \
    do {                                                              \
        if (!(cond)) {                                                \
            std::printf("FAIL %s:%d: ", __func__, __LINE__);          \
            std::printf(__VA_ARGS__);                                 \
            std::printf("\n");                                        \
            ++g_failures;                                             \
        }                                                             \
    } while (0)

constexpr uint32_t BLOCK_SIZE = 192;
constexpr uint32_t SAMPLE_RATE = 44100;
constexpr size_t NUM_FRAMES = 5;
constexpr int16_t FRAME_VALUES[NUM_FRAMES] = {0x1234, 0x2345, 0x3456, 0x4567, 0x5678};

// How the coded frame number in a synthetic frame header is encoded
enum class CodedNumberVariant {
    VALID,               // minimal 1-byte encoding
    INVALID_LEAD_FF,     // 0xFF is not a legal leading byte
    INVALID_CONTINUATION,// leading byte 0xC2 followed by a non-10xxxxxx byte
    SEVEN_BYTE_FORM,     // 0xFE form only valid for variable-block-size streams
};

struct SyntheticStream {
    std::vector<uint8_t> bytes;
    size_t frame_offsets[NUM_FRAMES + 1]{};  // last entry = end of stream
};

void append_be(std::vector<uint8_t>& out, uint64_t value, size_t num_bytes) {
    for (size_t i = 0; i < num_bytes; ++i) {
        out.push_back(static_cast<uint8_t>(value >> (8 * (num_bytes - 1 - i))));
    }
}

void append_frame(std::vector<uint8_t>& out, uint32_t frame_number, int16_t value,
                  CodedNumberVariant variant) {
    std::vector<uint8_t> frame;
    frame.push_back(0xFF);
    frame.push_back(0xF8);  // sync (15 bits) + mandatory 0 + fixed block size strategy
    frame.push_back(0x19);  // block size code 1 (192) | sample rate code 9 (44100)
    frame.push_back(0x08);  // channel assignment 0 (mono) | bit depth code 4 (16) | 0

    switch (variant) {
        case CodedNumberVariant::VALID:
            frame.push_back(static_cast<uint8_t>(frame_number));  // frame_number <= 0x7F
            break;
        case CodedNumberVariant::INVALID_LEAD_FF:
            frame.push_back(0xFF);
            for (size_t i = 0; i < 6; ++i) {
                frame.push_back(static_cast<uint8_t>(0x80 + i));  // valid continuations
            }
            break;
        case CodedNumberVariant::INVALID_CONTINUATION:
            frame.push_back(0xC2);
            frame.push_back(0x00);  // not 10xxxxxx
            break;
        case CodedNumberVariant::SEVEN_BYTE_FORM:
            frame.push_back(0xFE);
            for (size_t i = 0; i < 6; ++i) {
                frame.push_back(static_cast<uint8_t>(0x80 + i));  // valid continuations
            }
            break;
    }

    frame.push_back(calculate_crc8(frame.data(), frame.size()));

    // Constant subframe: header byte (type 0, no wasted bits) + 16-bit value.
    // All fields are whole bytes, so the frame needs no alignment padding.
    frame.push_back(0x00);
    append_be(frame, static_cast<uint16_t>(value), 2);

    append_be(frame, update_crc16(0, frame.data(), frame.size()), 2);
    out.insert(out.end(), frame.begin(), frame.end());
}

SyntheticStream build_stream(CodedNumberVariant first_frame_variant) {
    SyntheticStream stream;
    std::vector<uint8_t>& out = stream.bytes;

    out.insert(out.end(), {'f', 'L', 'a', 'C'});
    out.push_back(0x80);  // last metadata block | type 0 (STREAMINFO)
    append_be(out, 34, 3);

    append_be(out, BLOCK_SIZE, 2);  // min block size
    append_be(out, BLOCK_SIZE, 2);  // max block size
    append_be(out, 0, 3);           // min frame size (unknown)
    append_be(out, 0, 3);           // max frame size (unknown)
    // sample_rate(20) | channels-1(3) | bps-1(5) | total_samples(36)
    // The channels-1 term is a literal 0 (mono), kept spelled out so the OR chain lines
    // up one-to-one with the bit-layout comment above.
    // cppcheck-suppress badBitmaskCheck
    uint64_t packed = (static_cast<uint64_t>(SAMPLE_RATE) << 44) | (0ULL << 41) | (15ULL << 36) |
                      (NUM_FRAMES * BLOCK_SIZE);
    append_be(out, packed, 8);
    for (size_t i = 0; i < 16; ++i) {
        out.push_back(0x00);  // MD5 unknown
    }

    for (size_t i = 0; i < NUM_FRAMES; ++i) {
        stream.frame_offsets[i] = out.size();
        CodedNumberVariant variant =
            (i == 0) ? first_frame_variant : CodedNumberVariant::VALID;
        append_frame(out, static_cast<uint32_t>(i), FRAME_VALUES[i], variant);
    }
    stream.frame_offsets[NUM_FRAMES] = out.size();
    return stream;
}

struct DecodeRun {
    std::vector<int16_t> frame_values;  // first sample of each decoded frame
    std::vector<FLACDecoderResult> errors;
    std::vector<size_t> error_resume_positions;  // stream position after each error
    bool header_ready = false;
    bool invariant_violated = false;
};

// Feed the stream in fixed-size chunks, following the documented recovery
// contract: after an error, advance by bytes_consumed and keep decoding.
DecodeRun run_decoder(const std::vector<uint8_t>& stream, size_t chunk_size) {
    DecodeRun run;
    FLACDecoder decoder;
    std::vector<uint8_t> output(BLOCK_SIZE * sizeof(int16_t));

    size_t pos = 0;
    size_t stalled_calls = 0;
    while (pos < stream.size() && run.errors.size() < 32 && stalled_calls < 3) {
        size_t len = std::min(chunk_size, stream.size() - pos);
        size_t bytes_consumed = 0;
        size_t samples_decoded = 0;
        FLACDecoderResult result = decoder.decode(stream.data() + pos, len, output.data(),
                                                  output.size(), bytes_consumed, samples_decoded);

        if (bytes_consumed > len) {
            std::printf("FAIL: bytes_consumed %zu exceeds input length %zu (result %d)\n",
                        bytes_consumed, len, static_cast<int>(result));
            run.invariant_violated = true;
            return run;
        }
        stalled_calls = (bytes_consumed == 0) ? stalled_calls + 1 : 0;
        pos += bytes_consumed;

        if (result == micro_flac::FLAC_DECODER_HEADER_READY) {
            run.header_ready = true;
        } else if (result == micro_flac::FLAC_DECODER_SUCCESS) {
            int16_t first_sample = 0;
            std::memcpy(&first_sample, output.data(), sizeof(first_sample));
            run.frame_values.push_back(first_sample);
        } else if (result < 0) {
            run.errors.push_back(result);
            run.error_resume_positions.push_back(pos);
        } else if (result == micro_flac::FLAC_DECODER_END_OF_STREAM) {
            break;
        }
    }
    return run;
}

std::string values_to_string(const std::vector<int16_t>& values) {
    std::string s;
    for (int16_t v : values) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%04X ", static_cast<uint16_t>(v));
        s += buf;
    }
    return s;
}

void test_baseline_decode(size_t chunk_size) {
    SyntheticStream stream = build_stream(CodedNumberVariant::VALID);
    DecodeRun run = run_decoder(stream.bytes, chunk_size);

    EXPECT(!run.invariant_violated, "bytes_consumed invariant violated (chunk %zu)", chunk_size);
    EXPECT(run.header_ready, "no HEADER_READY (chunk %zu)", chunk_size);
    EXPECT(run.errors.empty(), "unexpected errors on clean stream (chunk %zu)", chunk_size);
    EXPECT(run.frame_values.size() == NUM_FRAMES, "decoded %zu/%zu frames (chunk %zu): %s",
           run.frame_values.size(), NUM_FRAMES, chunk_size,
           values_to_string(run.frame_values).c_str());
    for (size_t i = 0; i < run.frame_values.size() && i < NUM_FRAMES; ++i) {
        EXPECT(run.frame_values[i] == FRAME_VALUES[i], "frame %zu value 0x%04X != 0x%04X", i,
               static_cast<uint16_t>(run.frame_values[i]),
               static_cast<uint16_t>(FRAME_VALUES[i]));
    }
}

void test_crc_error_recovery(size_t chunk_size) {
    SyntheticStream stream = build_stream(CodedNumberVariant::VALID);

    // Corrupt the constant-subframe payload of frame 2 (0-indexed):
    // frame layout = 6-byte header + [subframe header, value hi, value lo] + CRC-16.
    constexpr size_t CORRUPT_FRAME = 2;
    stream.bytes[stream.frame_offsets[CORRUPT_FRAME] + 7] ^= 0x01;

    DecodeRun run = run_decoder(stream.bytes, chunk_size);

    EXPECT(!run.invariant_violated, "bytes_consumed invariant violated (chunk %zu)", chunk_size);
    EXPECT(run.errors.size() == 1, "expected 1 error, got %zu (chunk %zu)", run.errors.size(),
           chunk_size);
    if (run.errors.size() == 1) {
        EXPECT(run.errors[0] == micro_flac::FLAC_DECODER_ERROR_CRC_MISMATCH,
               "expected CRC_MISMATCH, got %d (chunk %zu)", static_cast<int>(run.errors[0]),
               chunk_size);
        // The recovery contract only works if the failed decode's bytes_consumed
        // lands exactly on the next frame's sync code.
        EXPECT(run.error_resume_positions[0] == stream.frame_offsets[CORRUPT_FRAME + 1],
               "error resume position %zu != next frame offset %zu (chunk %zu)",
               run.error_resume_positions[0], stream.frame_offsets[CORRUPT_FRAME + 1],
               chunk_size);
    }

    std::vector<int16_t> expected;
    for (size_t i = 0; i < NUM_FRAMES; ++i) {
        if (i != CORRUPT_FRAME) {
            expected.push_back(FRAME_VALUES[i]);
        }
    }
    EXPECT(run.frame_values == expected, "recovered frames [%s] != expected [%s] (chunk %zu)",
           values_to_string(run.frame_values).c_str(), values_to_string(expected).c_str(),
           chunk_size);
}

void test_malformed_coded_number(CodedNumberVariant variant, const char* name) {
    SyntheticStream stream = build_stream(variant);
    DecodeRun run = run_decoder(stream.bytes, stream.bytes.size());

    EXPECT(!run.invariant_violated, "bytes_consumed invariant violated (%s)", name);
    EXPECT(!run.errors.empty(), "%s: malformed coded number was accepted", name);
    if (!run.errors.empty()) {
        EXPECT(run.errors[0] == micro_flac::FLAC_DECODER_ERROR_BAD_HEADER,
               "%s: expected BAD_HEADER, got %d", name, static_cast<int>(run.errors[0]));
    }
    // The malformed frame must never surface as decoded audio.
    for (int16_t value : run.frame_values) {
        EXPECT(value != FRAME_VALUES[0], "%s: malformed frame was decoded", name);
    }
}

}  // namespace

int main() {
    test_baseline_decode(SIZE_MAX);  // one-shot
    test_baseline_decode(7);
    test_baseline_decode(1);

    test_crc_error_recovery(SIZE_MAX);
    test_crc_error_recovery(7);
    test_crc_error_recovery(1);

    test_malformed_coded_number(CodedNumberVariant::INVALID_LEAD_FF, "lead_0xFF");
    test_malformed_coded_number(CodedNumberVariant::INVALID_CONTINUATION, "bad_continuation");
    test_malformed_coded_number(CodedNumberVariant::SEVEN_BYTE_FORM, "7byte_in_fixed");

    if (g_failures == 0) {
        std::printf("All error-recovery regression tests passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", g_failures);
    return 1;
}
