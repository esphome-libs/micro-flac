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

#include "file_io.h"      // host_examples/common/include, shared with flac_to_wav
#include "flac_format.h"  // Channel assignment codes, MAGIC_BYTES, STREAMINFO_SIZE
#include "md5.h"          // host_examples/common/include, shared with flac_to_wav
#include "micro_flac/flac_encoder.h"
#include "micro_flac/pcm_format.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace micro_flac;

namespace {

constexpr uint32_t DEFAULT_BLOCK_SIZE = 4096;

// 8-bit WAV PCM is unsigned; flipping the top bit makes it PcmFormat's signed bytes
constexpr uint8_t WAV_8BIT_SIGN_FLIP = 0x80;

// STREAMINFO's MD5 signature is the header's last 16 bytes (RFC 9639 SS8.2)
constexpr size_t MD5_SIGNATURE_BYTES = 16;
constexpr size_t STREAMINFO_MD5_OFFSET = FLACEncoder::HEADER_BYTES - MD5_SIGNATURE_BYTES;
static_assert(FLACEncoder::HEADER_BYTES == sizeof(MAGIC_BYTES) + 4 + STREAMINFO_SIZE,
              "the MD5 offset assumes the header is exactly fLaC plus STREAMINFO");

constexpr uint16_t WAVE_FORMAT_PCM = 1;
constexpr uint16_t WAVE_FORMAT_EXTENSIBLE = 0xFFFE;
constexpr size_t FMT_CHUNK_SCRATCH_SIZE = 40;  // enough for the WAVE_FORMAT_EXTENSIBLE variant
constexpr size_t WAVE_FORMAT_EXTENSIBLE_MIN_SIZE = 40;

// ============================================================================
// Minimal WAV reader (8/16/24-bit PCM, 1-8 channels)
// ============================================================================

struct WavInfo {
    uint32_t sample_rate{0};
    uint16_t num_channels{0};
    uint16_t bits_per_sample{0};
    long data_offset{0};
    uint64_t data_size{0};
};

// Reads a RIFF/WAVE header, walking chunks (in any order, skipping unknown
// ones) until both "fmt " and "data" have been found. Understands standard
// PCM (format 1) and WAVE_FORMAT_EXTENSIBLE (format 0xFFFE) fmt chunks;
// rejects anything else (compressed formats, float PCM, depths other than
// 8/16/24, more than 8 channels). Leaves the file position unspecified; callers must
// std::fseek() to info.data_offset before reading samples.
bool read_wav_header(FILE* f, WavInfo& info) {
    char riff_id[4];
    uint32_t riff_size = 0;
    char wave_id[4];
    if (std::fread(riff_id, 1, 4, f) != 4 || std::memcmp(riff_id, "RIFF", 4) != 0) {
        return false;
    }
    if (std::fread(&riff_size, 4, 1, f) != 1) {
        return false;
    }
    if (std::fread(wave_id, 1, 4, f) != 4 || std::memcmp(wave_id, "WAVE", 4) != 0) {
        return false;
    }

    bool have_fmt = false;
    bool have_data = false;
    uint16_t audio_format = 0;

    while (!(have_fmt && have_data)) {
        char chunk_id[4];
        uint32_t chunk_size = 0;
        if (std::fread(chunk_id, 1, 4, f) != 4) {
            break;
        }
        if (std::fread(&chunk_size, 4, 1, f) != 1) {
            break;
        }
        const uint32_t pad = (chunk_size % 2 != 0) ? 1 : 0;

        if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
            uint8_t fmt_buf[FMT_CHUNK_SCRATCH_SIZE] = {};
            const uint32_t to_read =
                (chunk_size < FMT_CHUNK_SCRATCH_SIZE) ? chunk_size : FMT_CHUNK_SCRATCH_SIZE;
            if (std::fread(fmt_buf, 1, to_read, f) != to_read) {
                return false;
            }
            // Reachable whenever the fmt chunk exceeds FMT_CHUNK_SCRATCH_SIZE
            // (e.g. WAVE_FORMAT_EXTENSIBLE's 40-byte body); cppcheck's
            // value-flow analysis loses that range across the ternary above.
            // cppcheck-suppress knownConditionTrueFalse
            if (chunk_size > to_read) {
                std::fseek(f, static_cast<long>(chunk_size - to_read), SEEK_CUR);
            }
            if (pad != 0) {
                std::fseek(f, 1, SEEK_CUR);
            }

            // fmt chunk layout (RIFF, little-endian): audio_format(2),
            // num_channels(2), sample_rate(4), byte_rate(4), block_align(2),
            // bits_per_sample(2), [cbSize(2), valid_bits(2), channel_mask(4),
            // sub_format_guid(16)] for WAVE_FORMAT_EXTENSIBLE.
            // NOLINTBEGIN(readability-magic-numbers) -- byte offsets per RIFF "fmt " chunk layout
            // above
            audio_format =
                static_cast<uint16_t>(fmt_buf[0] | (static_cast<uint16_t>(fmt_buf[1]) << 8));
            info.num_channels =
                static_cast<uint16_t>(fmt_buf[2] | (static_cast<uint16_t>(fmt_buf[3]) << 8));
            info.sample_rate = static_cast<uint32_t>(fmt_buf[4]) |
                               (static_cast<uint32_t>(fmt_buf[5]) << 8) |
                               (static_cast<uint32_t>(fmt_buf[6]) << 16) |
                               (static_cast<uint32_t>(fmt_buf[7]) << 24);
            info.bits_per_sample =
                static_cast<uint16_t>(fmt_buf[14] | (static_cast<uint16_t>(fmt_buf[15]) << 8));

            if (audio_format == WAVE_FORMAT_EXTENSIBLE &&
                chunk_size >= WAVE_FORMAT_EXTENSIBLE_MIN_SIZE) {
                // Sub-format GUID starts at byte 24; its first 2 bytes are the
                // real (legacy) format code.
                audio_format =
                    static_cast<uint16_t>(fmt_buf[24] | (static_cast<uint16_t>(fmt_buf[25]) << 8));
            }
            // NOLINTEND(readability-magic-numbers)
            have_fmt = true;
        } else if (std::memcmp(chunk_id, "data", 4) == 0) {
            info.data_offset = std::ftell(f);
            info.data_size = chunk_size;
            have_data = true;
            if (!have_fmt) {
                // fmt hasn't been seen yet (unusual chunk order); skip over
                // the PCM payload itself to keep walking chunks.
                std::fseek(f, static_cast<long>(chunk_size) + static_cast<long>(pad), SEEK_CUR);
            }
        } else {
            std::fseek(f, static_cast<long>(chunk_size) + static_cast<long>(pad), SEEK_CUR);
        }

        if (std::feof(f) && !(have_fmt && have_data)) {
            break;
        }
    }

    if (!have_fmt || !have_data) {
        return false;
    }
    if (audio_format != WAVE_FORMAT_PCM) {
        std::fprintf(stderr,
                     "Error: unsupported WAV audio format code %u (only PCM is supported)\n",
                     audio_format);
        return false;
    }
    if (info.bits_per_sample != 8 && info.bits_per_sample != 16 && info.bits_per_sample != 24) {
        std::fprintf(stderr,
                     "Error: unsupported bit depth %u (8, 16 and 24-bit PCM are supported)\n",
                     info.bits_per_sample);
        return false;
    }
    if (info.num_channels < 1 || info.num_channels > MAX_CHANNELS) {
        std::fprintf(stderr, "Error: unsupported channel count %u (1-%u are supported)\n",
                     info.num_channels, MAX_CHANNELS);
        return false;
    }

    // A WAV streamed to a pipe can't seek back to fill in its sizes, so its
    // data chunk declares 0xFFFFFFFF (or any size past EOF); encode what the
    // file actually holds.
    if (std::fseek(f, 0, SEEK_END) == 0) {
        const long file_end = std::ftell(f);
        if (file_end >= info.data_offset) {
            const uint64_t available = static_cast<uint64_t>(file_end - info.data_offset);
            if (info.data_size > available) {
                info.data_size = available;
            }
        }
    }
    return true;
}

// ============================================================================
// Command line parsing
// ============================================================================

struct Args {
    uint32_t block_size = DEFAULT_BLOCK_SIZE;
    bool lpc = false;
    bool lpc_stereo_search = false;
    uint8_t max_lpc_order = FLACEncoderOptions{}.max_lpc_order;
    uint8_t max_rice_partition_order = 0;
    bool wasted_bits = FLACEncoderOptions{}.wasted_bits;
    const char* input_file = nullptr;
    const char* output_file = nullptr;
};

// Returns true on success, false on error (error messages already printed)
bool parse_args(int argc, const char* const argv[], Args& args) {
    int arg_idx = 1;
    while (arg_idx < argc) {
        const char* arg = argv[arg_idx];
        if (std::strcmp(arg, "--block-size") == 0) {
            if (arg_idx + 1 >= argc) {
                std::fprintf(stderr, "Error: --block-size requires a value\n");
                return false;
            }
            char* end = nullptr;
            const long value = std::strtol(argv[arg_idx + 1], &end, 10);
            if (end == argv[arg_idx + 1] || *end != '\0' || value < FLACEncoder::MIN_BLOCK_SIZE ||
                value > FLACEncoder::MAX_BLOCK_SIZE) {
                std::fprintf(stderr, "Error: invalid --block-size '%s' (must be %u-%u)\n",
                             argv[arg_idx + 1], FLACEncoder::MIN_BLOCK_SIZE,
                             FLACEncoder::MAX_BLOCK_SIZE);
                return false;
            }
            args.block_size = static_cast<uint32_t>(value);
            arg_idx += 2;
        } else if (std::strcmp(arg, "--lpc") == 0) {
            args.lpc = true;
            arg_idx += 1;
        } else if (std::strcmp(arg, "--lpc-stereo-search") == 0) {
            args.lpc = true;  // Implies --lpc
            args.lpc_stereo_search = true;
            arg_idx += 1;
        } else if (std::strcmp(arg, "--lpc-order") == 0) {
            if (arg_idx + 1 >= argc) {
                std::fprintf(stderr, "Error: --lpc-order requires a value\n");
                return false;
            }
            char* end = nullptr;
            const long value = std::strtol(argv[arg_idx + 1], &end, 10);
            if (end == argv[arg_idx + 1] || *end != '\0' || value < 1 ||
                value > FLACEncoder::MAX_LPC_ORDER) {
                std::fprintf(stderr, "Error: invalid --lpc-order '%s' (must be 1-%u)\n",
                             argv[arg_idx + 1], static_cast<unsigned>(FLACEncoder::MAX_LPC_ORDER));
                return false;
            }
            args.lpc = true;  // Setting an order implies --lpc
            args.max_lpc_order = static_cast<uint8_t>(value);
            arg_idx += 2;
        } else if (std::strcmp(arg, "--partition-order") == 0) {
            if (arg_idx + 1 >= argc) {
                std::fprintf(stderr, "Error: --partition-order requires a value\n");
                return false;
            }
            char* end = nullptr;
            const long value = std::strtol(argv[arg_idx + 1], &end, 10);
            if (end == argv[arg_idx + 1] || *end != '\0' || value < 0 ||
                value > FLACEncoder::MAX_RICE_PARTITION_ORDER) {
                std::fprintf(stderr, "Error: invalid --partition-order '%s' (must be 0-%u)\n",
                             argv[arg_idx + 1],
                             static_cast<unsigned>(FLACEncoder::MAX_RICE_PARTITION_ORDER));
                return false;
            }
            args.max_rice_partition_order = static_cast<uint8_t>(value);
            arg_idx += 2;
        } else if (std::strcmp(arg, "--no-wasted-bits") == 0) {
            args.wasted_bits = false;
            arg_idx += 1;
        } else {
            break;
        }
    }

    if (argc - arg_idx != 2) {
        std::fprintf(stderr,
                     "Usage: %s [--block-size N] [--lpc] [--lpc-order N] [--lpc-stereo-search] "
                     "[--partition-order N] [--no-wasted-bits] <input.wav> <output.flac>\n",
                     argv[0]);
        return false;
    }

    args.input_file = argv[arg_idx];
    args.output_file = argv[arg_idx + 1];
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {  // NOLINT(bugprone-exception-escape)
    Args args;
    if (!parse_args(argc, argv, args)) {
        return 1;
    }

    FILE* wav_file = std::fopen(args.input_file, "rb");
    if (!wav_file) {
        std::fprintf(stderr, "Error: could not open input file: %s\n", args.input_file);
        return 1;
    }
    if (is_same_file(args.output_file, args.input_file)) {
        std::fprintf(stderr, "Error: the output file is the input file: %s\n", args.output_file);
        std::fclose(wav_file);
        return 1;
    }

    WavInfo wav_info;
    if (!read_wav_header(wav_file, wav_info)) {
        std::fprintf(stderr,
                     "Error: '%s' is not a supported WAV file (must be 8/16/24-bit PCM, 1-%u "
                     "channels)\n",
                     args.input_file, MAX_CHANNELS);
        std::fclose(wav_file);
        return 1;
    }
    std::fseek(wav_file, wav_info.data_offset, SEEK_SET);

    // WAV's PCM layout is already PcmFormat's: interleaved, little-endian,
    // whole bytes per sample. Only 8-bit data needs touching (see
    // WAV_8BIT_SIGN_FLIP).
    const PcmFormat format(wav_info.sample_rate, wav_info.num_channels, wav_info.bits_per_sample);

    const uint64_t bytes_per_frame = format.bytes_per_frame();
    const uint64_t total_samples_per_channel = wav_info.data_size / bytes_per_frame;
    if (total_samples_per_channel == 0) {
        std::fprintf(stderr, "Error: '%s' contains no audio samples\n", args.input_file);
        std::fclose(wav_file);
        return 1;
    }

    FLACEncoderOptions options;
    options.block_size = args.block_size;
    options.lpc = args.lpc;
    options.max_lpc_order = args.max_lpc_order;
    options.lpc_stereo_search = args.lpc_stereo_search;
    options.max_rice_partition_order = args.max_rice_partition_order;
    options.wasted_bits = args.wasted_bits;

    FLACEncoder encoder(format, options);

    // One output buffer of get_max_output_bytes() covers the header and every
    // frame; one input buffer of get_input_block_bytes() holds exactly the block
    // each encode() call consumes. Both are 0 for an unsupported
    // configuration, which the first write_header() call reports.
    std::vector<uint8_t> out_buffer(encoder.get_max_output_bytes());
    std::vector<uint8_t> in_buffer(encoder.get_input_block_bytes());

    size_t bytes_written = 0;
    FLACEncoderResult result =
        encoder.write_header(out_buffer.data(), out_buffer.size(), bytes_written);
    if (result != FLAC_ENCODER_SUCCESS) {
        std::fprintf(stderr, "Error: encoder rejected the configuration (code %d)\n",
                     static_cast<int>(result));
        std::fclose(wav_file);
        return 1;
    }

    FILE* flac_file = std::fopen(args.output_file, "wb");
    if (!flac_file) {
        std::fprintf(stderr, "Error: could not create output file: %s\n", args.output_file);
        std::fclose(wav_file);
        return 1;
    }
    if (!write_all(flac_file, out_buffer.data(), bytes_written)) {
        std::fprintf(stderr, "Error: could not write output file: %s\n", args.output_file);
        std::fclose(wav_file);
        std::fclose(flac_file);
        return 1;
    }

    // The stream's MD5 signature, which FLACEncoder leaves to its caller
    // (STREAMINFO's field stays zero, "unknown", otherwise). RFC 9639 SS8.2
    // defines it over the samples interleaved, signed, little-endian, in
    // whole bytes: exactly PcmFormat's layout for the byte-aligned depths a
    // WAV file carries, so each block is hashed as read (after the 8-bit sign
    // flip) and patched into the finished header at the end.
    MD5 md5;

    std::printf("Encoding...\n");
    std::printf("  Input: %s\n", args.input_file);
    std::printf("  Sample rate: %u Hz\n", wav_info.sample_rate);
    std::printf("  Channels: %u\n", wav_info.num_channels);
    std::printf("  Bit depth: %u\n", wav_info.bits_per_sample);
    std::printf("  Total samples per channel: %llu\n",
                static_cast<unsigned long long>(total_samples_per_channel));
    std::printf("  Block size: %u\n", args.block_size);

    uint64_t total_output_bytes = bytes_written;
    size_t largest_frame_bytes = 0;
    uint32_t frame_count = 0;
    uint64_t data_remaining = total_samples_per_channel * bytes_per_frame;
    bool finished = false;

    // Each stereo frame's channel assignment, read back from its header's
    // byte 3 (RFC 9639 SS9.1.3)
    uint32_t assignment_count_independent = 0;
    uint32_t assignment_count_left_side = 0;
    uint32_t assignment_count_right_side = 0;
    uint32_t assignment_count_mid_side = 0;

    // Each pass reads one block: a full one goes to encode(), a short one (or
    // nothing, at the end of the input) to finish().
    while (!finished) {
        const size_t want = (data_remaining < in_buffer.size())
                                ? static_cast<size_t>(data_remaining)
                                : in_buffer.size();
        const size_t got = (want > 0) ? std::fread(in_buffer.data(), 1, want, wav_file) : 0;
        if (got != want) {
            std::fprintf(stderr,
                         "Error: unexpected end of WAV data (expected %zu bytes, got %zu)\n", want,
                         got);
            std::fclose(wav_file);
            std::fclose(flac_file);
            return 1;
        }
        data_remaining -= got;
        if (format.bits_per_sample() == 8) {
            for (size_t i = 0; i < got; i++) {
                in_buffer[i] ^= WAV_8BIT_SIGN_FLIP;
            }
        }
        md5.update(in_buffer.data(), got);

        if (got == in_buffer.size()) {
            size_t bytes_consumed = 0;
            result = encoder.encode(in_buffer.data(), got, out_buffer.data(), out_buffer.size(),
                                    bytes_consumed, bytes_written);
        } else {
            result = encoder.finish(in_buffer.data(), got, out_buffer.data(), out_buffer.size(),
                                    bytes_written);
            finished = true;
        }
        if (result != FLAC_ENCODER_SUCCESS) {
            std::fprintf(stderr, "Error: %s() failed with code %d at sample %llu\n",
                         finished ? "finish" : "encode", static_cast<int>(result),
                         static_cast<unsigned long long>(encoder.get_total_samples_encoded()));
            std::fclose(wav_file);
            std::fclose(flac_file);
            return 1;
        }
        if (bytes_written == 0) {
            continue;  // finish() with nothing left over writes no frame
        }

        if (wav_info.num_channels == 2) {
            const uint8_t assignment_code = static_cast<uint8_t>(out_buffer[3] >> 4);
            switch (assignment_code) {
                case CHANNEL_INDEPENDENT_STEREO:
                    assignment_count_independent++;
                    break;
                case CHANNEL_LEFT_SIDE:
                    assignment_count_left_side++;
                    break;
                case CHANNEL_RIGHT_SIDE:
                    assignment_count_right_side++;
                    break;
                case CHANNEL_MID_SIDE:
                    assignment_count_mid_side++;
                    break;
                default:
                    break;
            }
        }

        if (!write_all(flac_file, out_buffer.data(), bytes_written)) {
            std::fprintf(stderr, "Error: could not write output file: %s\n", args.output_file);
            std::fclose(wav_file);
            std::fclose(flac_file);
            return 1;
        }
        total_output_bytes += bytes_written;
        if (bytes_written > largest_frame_bytes) {
            largest_frame_bytes = bytes_written;
        }
        frame_count++;
    }
    std::fclose(wav_file);

    // Replace the provisional header with the finished one
    result = encoder.write_header(out_buffer.data(), out_buffer.size(), bytes_written);
    if (result != FLAC_ENCODER_SUCCESS || std::fseek(flac_file, 0, SEEK_SET) != 0) {
        std::fprintf(stderr, "Error: could not rewrite the finished stream header\n");
        std::fclose(flac_file);
        return 1;
    }
    const std::array<uint8_t, MD5_SIGNATURE_BYTES> md5_signature = md5.finalize();
    std::memcpy(out_buffer.data() + STREAMINFO_MD5_OFFSET, md5_signature.data(),
                md5_signature.size());
    const bool header_written = write_all(flac_file, out_buffer.data(), bytes_written);
    if (!close_checked(flac_file) || !header_written) {
        std::fprintf(stderr, "Error: could not write output file: %s\n", args.output_file);
        return 1;
    }

    const uint64_t samples_encoded = encoder.get_total_samples_encoded();
    const uint64_t total_samples_all_channels = samples_encoded * wav_info.num_channels;
    // Divisor below is guarded by the "total_samples_all_channels > 0" branch.
    const double bits_per_sample = total_samples_all_channels > 0
                                       // NOLINTNEXTLINE(clang-analyzer-optin.taint.TaintedDiv)
                                       ? (static_cast<double>(total_output_bytes) * 8.0 /
                                          static_cast<double>(total_samples_all_channels))
                                       : 0.0;

    std::printf("Done.\n");
    std::printf("Frames written: %u\n", frame_count);
    std::printf("Samples per channel encoded: %llu\n",
                static_cast<unsigned long long>(samples_encoded));
    std::printf("Output file: %s\n", args.output_file);
    std::printf("MD5 signature: ");
    for (const uint8_t byte : md5_signature) {
        std::printf("%02x", byte);
    }
    std::printf("\n");
    std::printf("Output bytes: %llu\n", static_cast<unsigned long long>(total_output_bytes));
    std::printf("Bits per sample (average, all channels): %.3f\n", bits_per_sample);
    std::printf("Largest frame: %zu bytes (max_output_bytes bound: %zu bytes)\n",
                largest_frame_bytes, encoder.get_max_output_bytes());
    std::printf("Compression ratio vs %u-bit PCM: %.3f\n", wav_info.bits_per_sample,
                bits_per_sample > 0.0
                    ? static_cast<double>(wav_info.bits_per_sample) / bits_per_sample
                    : 0.0);
    if (wav_info.num_channels == 2) {
        std::printf(
            "Channel assignment counts: independent=%u left/side=%u right/side=%u mid/side=%u\n",
            assignment_count_independent, assignment_count_left_side, assignment_count_right_side,
            assignment_count_mid_side);
    }

    return 0;
}
