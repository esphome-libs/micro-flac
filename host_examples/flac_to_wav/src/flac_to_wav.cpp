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

#include "md5.h"
#include "micro_flac/flac_decoder.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace micro_flac;

// Named constants to avoid magic numbers
namespace {
constexpr uint16_t BIT_DEPTH_12 = 12;
constexpr uint16_t BIT_DEPTH_20 = 20;
constexpr uint16_t BIT_DEPTH_24 = 24;
constexpr uint16_t BIT_DEPTH_32 = 32;
constexpr uint16_t WAVE_FORMAT_EXTENSIBLE = 0xFFFE;
constexpr uint16_t WAV_EXTENSIBLE_CB_SIZE = 22;
constexpr uint32_t PROGRESS_UPDATE_INTERVAL_SECONDS = 10;
constexpr uint64_t PERCENTAGE_MULTIPLIER = 100;
}  // namespace

// Pack samples for MD5 computation according to FLAC spec
// Samples should be sign-extended to the next whole byte in little-endian order
void pack_samples_for_md5(const uint8_t* padded_samples, uint8_t* packed_output,
                          uint32_t num_samples, uint32_t bits_per_sample) {
    uint32_t bytes_per_padded_sample = (bits_per_sample + 7) / 8;
    uint32_t shift_amount = (bits_per_sample % 8 != 0) ? (8 - (bits_per_sample % 8)) : 0;

    // For byte-aligned samples, no repacking needed
    if (shift_amount == 0) {
        std::memcpy(packed_output, padded_samples,
                    static_cast<size_t>(num_samples) * bytes_per_padded_sample);
        return;
    }

    // For non-byte-aligned samples, unshift and sign-extend
    for (uint32_t i = 0; i < num_samples; i++) {
        const uint8_t* sample_ptr =
            padded_samples + (static_cast<size_t>(i) * bytes_per_padded_sample);
        uint8_t* output_ptr = packed_output + (static_cast<size_t>(i) * bytes_per_padded_sample);

        // Read the padded sample (LSB-padded by decoder)
        int32_t sample = 0;
        for (uint32_t byte = 0; byte < bytes_per_padded_sample; byte++) {
            sample |= ((int32_t)sample_ptr[byte]) << (byte * 8);
        }

        // Right-shift to remove padding
        sample >>= shift_amount;

        // Sign-extend to fill the container
        // Create a mask for the sign bit
        int32_t sign_bit = 1 << (bits_per_sample - 1);
        if (sample & sign_bit) {
            // Negative number - extend sign bits
            int32_t extension_mask = ~((1 << bits_per_sample) - 1);
            sample |= extension_mask;
        }

        // Write back in little-endian
        for (uint32_t byte = 0; byte < bytes_per_padded_sample; byte++) {
            output_ptr[byte] = static_cast<uint8_t>((sample >> (byte * 8)) & 0xFF);
        }
    }
}

struct WAVHeader {
    char riff[4];              // "RIFF"
    uint32_t file_size;        // File size - 8
    char wave[4];              // "WAVE"
    char fmt[4];               // "fmt "
    uint32_t fmt_size;         // Format chunk size (16 for PCM, 40 for EXTENSIBLE)
    uint16_t audio_format;     // Audio format (1 for PCM, 0xFFFE for EXTENSIBLE)
    uint16_t num_channels;     // Number of channels
    uint32_t sample_rate;      // Sample rate
    uint32_t byte_rate;        // Byte rate
    uint16_t block_align;      // Block align
    uint16_t bits_per_sample;  // Bits per sample (container size)
};

struct WAVExtensibleHeader {
    uint16_t cb_size;                // Size of extension (22)
    uint16_t valid_bits_per_sample;  // Valid bits in container
    uint32_t channel_mask;           // Channel position mask
    uint8_t sub_format[16];          // Format GUID (first 2 bytes are format code)
};

struct WAVDataChunk {
    char data[4];        // "data"
    uint32_t data_size;  // Data chunk size
};

void write_wav_header(FILE* file, uint32_t sample_rate, uint16_t num_channels,
                      uint16_t bits_per_sample, uint32_t num_samples) {
    WAVHeader header{};
    WAVExtensibleHeader ext_header{};
    WAVDataChunk data_chunk{};

    // Calculate container size (round up to byte boundary)
    uint16_t container_bits = static_cast<uint16_t>(((bits_per_sample + 7) / 8) * 8);
    uint32_t bytes_per_sample = container_bits / 8;

    // Determine if we need WAVE_FORMAT_EXTENSIBLE
    // Use extensible format for non-standard bit depths or high bit depths
    bool use_extensible = (bits_per_sample == BIT_DEPTH_12) || (bits_per_sample == BIT_DEPTH_20) ||
                          (bits_per_sample == BIT_DEPTH_24) || (bits_per_sample == BIT_DEPTH_32) ||
                          (num_channels > 2);

    // RIFF chunk
    std::memcpy(header.riff, "RIFF", 4);

    // WAVE format
    std::memcpy(header.wave, "WAVE", 4);

    // fmt chunk
    std::memcpy(header.fmt, "fmt ", 4);

    if (use_extensible) {
        // Use WAVE_FORMAT_EXTENSIBLE
        header.fmt_size = 40;  // Extended format
        header.audio_format = WAVE_FORMAT_EXTENSIBLE;
        header.num_channels = num_channels;
        header.sample_rate = sample_rate;
        header.bits_per_sample = container_bits;
        header.byte_rate = sample_rate * num_channels * bytes_per_sample;
        header.block_align = static_cast<uint16_t>(num_channels * bytes_per_sample);

        // Extension
        ext_header.cb_size = WAV_EXTENSIBLE_CB_SIZE;
        ext_header.valid_bits_per_sample = bits_per_sample;
        // Microsoft channel masks per FLAC spec channel assignments
        // 1: FC, 2: FL+FR, 3: FL+FR+FC, 4: FL+FR+BL+BR,
        // 5: FL+FR+FC+BL+BR, 6: FL+FR+FC+LFE+BL+BR,
        // 7: FL+FR+FC+LFE+BL+BR+BC, 8: FL+FR+FC+LFE+BL+BR+SL+SR
        static const uint32_t CHANNEL_MASKS[] = {
            0x4,    // 1ch: FC
            0x3,    // 2ch: FL | FR
            0x7,    // 3ch: FL | FR | FC
            0x33,   // 4ch: FL | FR | BL | BR
            0x37,   // 5ch: FL | FR | FC | BL | BR
            0x3F,   // 6ch: FL | FR | FC | LFE | BL | BR
            0x13F,  // 7ch: FL | FR | FC | LFE | BL | BR | BC
            0x63F,  // 8ch: FL | FR | FC | LFE | BL | BR | SL | SR
        };
        ext_header.channel_mask =
            (num_channels >= 1 && num_channels <= 8) ? CHANNEL_MASKS[num_channels - 1] : 0;

        // GUID for PCM: {00000001-0000-0010-8000-00aa00389b71}
        static const uint8_t PCM_GUID[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                             0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
        std::memcpy(ext_header.sub_format, PCM_GUID, 16);
    } else {
        // Use standard PCM format
        header.fmt_size = 16;     // PCM format
        header.audio_format = 1;  // PCM
        header.num_channels = num_channels;
        header.sample_rate = sample_rate;
        header.bits_per_sample = bits_per_sample;
        header.byte_rate = sample_rate * num_channels * bytes_per_sample;
        header.block_align = static_cast<uint16_t>(num_channels * bytes_per_sample);
    }

    // data chunk
    std::memcpy(data_chunk.data, "data", 4);
    data_chunk.data_size = num_samples * num_channels * bytes_per_sample;

    // Calculate file size
    uint32_t fmt_chunk_size = 8 + header.fmt_size;            // "fmt " + size + data
    uint32_t data_chunk_size = 8 + data_chunk.data_size;      // "data" + size + data
    header.file_size = 4 + fmt_chunk_size + data_chunk_size;  // "WAVE" + chunks

    // Write header
    std::fwrite(&header, sizeof(header), 1, file);
    if (use_extensible) {
        std::fwrite(&ext_header, sizeof(ext_header), 1, file);
    }
    std::fwrite(&data_chunk, sizeof(data_chunk), 1, file);
}

struct Args {
    size_t streaming_chunk_size = 65536;
    bool output_32bit = false;
    const char* input_file = nullptr;
    const char* output_file = nullptr;
};

// Returns true on success, false on error (error messages already printed)
bool parse_args(int argc, char* argv[], Args& args) {
    int arg_idx = 1;
    while (arg_idx < argc) {
        const char* arg = argv[arg_idx];
        if (std::strcmp(arg, "--streaming") == 0) {
            if (arg_idx + 1 >= argc) {
                std::fprintf(stderr, "Error: --streaming requires a chunk_size argument\n");
                return false;
            }
            char* end = nullptr;
            args.streaming_chunk_size =
                std::strtoul(argv[arg_idx + 1], &end, 10);  // NOLINT(readability-magic-numbers)
            if (end == argv[arg_idx + 1] || *end != '\0') {
                std::fprintf(stderr, "Error: invalid chunk_size '%s'\n", argv[arg_idx + 1]);
                return false;
            }
            if (args.streaming_chunk_size == 0) {
                std::fprintf(stderr, "Error: chunk_size must be > 0\n");
                return false;
            }
            arg_idx += 2;
        } else if (std::strcmp(arg, "--output-32bit") == 0) {
            args.output_32bit = true;
            arg_idx += 1;
        } else {
            break;
        }
    }

    if (argc - arg_idx != 2) {
        std::fprintf(stderr,
                     "Usage: %s [--streaming <chunk_size>] [--output-32bit] "
                     "<input.flac|.ogg|.oga> <output.wav>\n",
                     argv[0]);
        return false;
    }

    args.input_file = argv[arg_idx];
    args.output_file = argv[arg_idx + 1];
    return true;
}

void update_wav_header(const char* output_file, long header_end_pos,
                       uint32_t samples_per_channel_decoded, uint32_t num_channels,
                       uint32_t bytes_per_sample) {
    FILE* f = std::fopen(output_file, "r+b");
    if (f) {
        uint32_t data_size = samples_per_channel_decoded * num_channels * bytes_per_sample;
        uint32_t file_size = static_cast<uint32_t>(header_end_pos) - 8 + data_size;

        std::fseek(f, 4, SEEK_SET);
        std::fwrite(&file_size, 4, 1, f);

        std::fseek(f, header_end_pos - 4, SEEK_SET);
        std::fwrite(&data_size, 4, 1, f);
        std::fclose(f);
    }
}

void verify_md5(MD5& md5_ctx, const uint8_t (&md5_sig)[16], bool md5_all_zero) {
    std::printf("\n=== MD5 Verification ===\n");
    if (md5_all_zero) {
        std::printf("Status: SKIPPED (no MD5 signature in file)\n");
        return;
    }

    auto computed_md5 = md5_ctx.finalize();

    bool md5_match = true;
    for (size_t i = 0; i < 16; i++) {
        if (computed_md5[i] != md5_sig[i]) {
            md5_match = false;
            break;
        }
    }

    std::printf("Expected MD5: ");
    for (unsigned char i : md5_sig) {
        std::printf("%02x", i);
    }
    std::printf("\n");

    std::printf("Computed MD5: ");
    for (size_t i = 0; i < 16; i++) {
        std::printf("%02x", computed_md5[i]);
    }
    std::printf("\n");

    if (md5_match) {
        std::printf("Result: PASS - MD5 signatures match!\n");
    } else {
        std::printf("Result: FAIL - MD5 signatures do NOT match!\n");
    }
}

int main(int argc, char* argv[]) {  // NOLINT(bugprone-exception-escape)
    Args args;
    if (!parse_args(argc, argv, args)) {
        return 1;
    }

    const char* input_file = args.input_file;
    const char* output_file = args.output_file;
    size_t streaming_chunk_size = args.streaming_chunk_size;
    bool output_32bit = args.output_32bit;

    // Open input file
    FILE* flac_file = std::fopen(input_file, "rb");
    if (!flac_file) {
        std::fprintf(stderr, "Error: Could not open input file: %s\n", input_file);
        return 1;
    }

    // Initialize FLAC decoder
    FLACDecoder decoder;
    decoder.set_crc_check_enabled(true);

    // State for WAV output
    FILE* wav_file = nullptr;
    long header_end_pos = 0;
    uint32_t sample_rate = 0;
    uint32_t num_channels = 0;
    uint32_t bits_per_sample = 0;
    uint64_t total_samples = 0;
    uint32_t bytes_per_sample_out = 0;
    uint8_t md5_sig[16]{};

    std::vector<uint8_t> output_buffer;
    std::vector<int32_t> output_buffer_32;
    size_t output_buffer_size = 0;

    // MD5 verification state
    MD5 md5_ctx;
    bool md5_all_zero = true;
    std::vector<uint8_t> md5_scratch_buffer;  // Pre-allocated for MD5 packing

    // Decode state
    uint32_t frames_decoded = 0;
    uint32_t samples_per_channel_decoded = 0;

    // Helper lambda to process decoded samples (MD5 update + WAV write)
    auto process_decoded_samples = [&](size_t num_samples) {
        uint32_t num_samples_u32 = static_cast<uint32_t>(num_samples);
        uint32_t bytes_to_write = num_samples_u32 * bytes_per_sample_out;

        if (!md5_all_zero) {
            if (output_32bit) {
                // 32-bit mode: right-shift samples back to native bit depth for MD5
                uint32_t native_bytes_per_sample = (bits_per_sample + 7) / 8;
                uint32_t md5_size = num_samples_u32 * native_bytes_per_sample;
                uint32_t shift = BIT_DEPTH_32 - bits_per_sample;

                for (uint32_t i = 0; i < num_samples_u32; i++) {
                    int32_t sample = output_buffer_32[i] >> shift;

                    // Pack to native byte width in little-endian
                    // For 8-bit: the right-shifted value is already signed (-128..127),
                    // which matches the FLAC MD5 spec when cast to uint8_t
                    for (uint32_t b = 0; b < native_bytes_per_sample; b++) {
                        md5_scratch_buffer[i * native_bytes_per_sample + b] =
                            static_cast<uint8_t>((sample >> (b * 8)) & 0xFF);
                    }
                }
                md5_ctx.update(md5_scratch_buffer.data(), md5_size);
            } else {
                pack_samples_for_md5(output_buffer.data(), md5_scratch_buffer.data(),
                                     num_samples_u32, bits_per_sample);
                md5_ctx.update(md5_scratch_buffer.data(), bytes_to_write);
            }
        }

        if (wav_file) {
            if (output_32bit) {
                std::fwrite(output_buffer_32.data(), 1, bytes_to_write, wav_file);
            } else {
                if (bits_per_sample == 8) {
                    for (uint32_t i = 0; i < bytes_to_write; i++) {
                        output_buffer[i] =
                            static_cast<uint8_t>(static_cast<int8_t>(output_buffer[i]) + 128);
                    }
                }
                std::fwrite(output_buffer.data(), 1, bytes_to_write, wav_file);
            }
        }

        uint32_t samples_this_frame = (num_channels > 0) ? (num_samples_u32 / num_channels) : 0;
        samples_per_channel_decoded += samples_this_frame;
        frames_decoded++;

        if (total_samples > 0 &&
            samples_per_channel_decoded % (sample_rate * PROGRESS_UPDATE_INTERVAL_SECONDS) == 0) {
            std::printf("  Decoded %u / %llu samples per channel (%llu%%)\n",
                        samples_per_channel_decoded, (unsigned long long)total_samples,
                        (unsigned long long)(static_cast<uint64_t>(samples_per_channel_decoded) *
                                             PERCENTAGE_MULTIPLIER / total_samples));
        }
    };

    // Main decode loop using unified decode() API
    std::vector<uint8_t> chunk_buffer(streaming_chunk_size);
    bool done = false;

    std::printf("Decoding...\n");
    std::printf("  Chunk size: %zu bytes\n", streaming_chunk_size);

    while (!done && !std::feof(flac_file)) {
        size_t chunk_len = std::fread(chunk_buffer.data(), 1, streaming_chunk_size, flac_file);
        if (chunk_len == 0) {
            break;
        }
        if (std::ferror(flac_file)) {
            std::fprintf(stderr, "Error: Failed to read input file\n");
            if (wav_file) {
                std::fclose(wav_file);
            }
            std::fclose(flac_file);
            return 1;
        }

        size_t offset = 0;
        while (offset < chunk_len) {
            size_t consumed = 0;
            size_t samples = 0;
            FLACDecoderResult result = FLAC_DECODER_ERROR_INTERNAL;
            if (output_32bit) {
                result =
                    decoder.decode(chunk_buffer.data() + offset, chunk_len - offset,
                                   output_buffer_32.data(), output_buffer_size, consumed, samples);
            } else {
                result =
                    decoder.decode(chunk_buffer.data() + offset, chunk_len - offset,
                                   output_buffer.data(), output_buffer_size, consumed, samples);
            }
            offset += consumed;

            switch (result) {
                case FLAC_DECODER_HEADER_READY: {
                    // Header is complete: get stream info and set up output
                    const FLACStreamInfo& stream_info = decoder.get_stream_info();
                    sample_rate = stream_info.sample_rate();
                    num_channels = stream_info.num_channels();
                    bits_per_sample = stream_info.bits_per_sample();
                    total_samples = stream_info.total_samples_per_channel();

                    size_t output_samples = decoder.get_output_buffer_size_samples();
                    if (output_32bit) {
                        bytes_per_sample_out = 4;
                        output_buffer_32.resize(output_samples);
                        output_buffer_size = output_samples;
                    } else {
                        bytes_per_sample_out = stream_info.bytes_per_sample();
                        output_buffer_size = output_samples * bytes_per_sample_out;
                        output_buffer.resize(output_buffer_size);
                    }

                    // Pre-allocate MD5 scratch buffer for reuse across frames
                    md5_scratch_buffer.resize(output_samples * bytes_per_sample_out);

                    std::printf("FLAC file info:\n");
                    std::printf("  Sample rate: %u Hz\n", sample_rate);
                    std::printf("  Channels: %u\n", num_channels);
                    std::printf("  Bits per sample: %u\n", bits_per_sample);
                    std::printf("  Total samples: %llu\n", (unsigned long long)total_samples);
                    std::printf("  Max block size: %u\n", stream_info.max_block_size());

                    std::memcpy(md5_sig, stream_info.md5_signature(), 16);
                    std::printf("  MD5 signature: ");
                    for (unsigned char i : md5_sig) {
                        std::printf("%02x", i);
                    }
                    std::printf("\n");

                    const auto& metadata = decoder.get_metadata_blocks();
                    std::printf("  Metadata blocks: %zu\n", metadata.size());
                    for (const auto& block : metadata) {
                        std::printf("    - Type %u, size: %u bytes\n", block.type, block.length);
                    }

                    // Check MD5 signature
                    md5_all_zero = true;
                    for (unsigned char i : md5_sig) {
                        if (i != 0) {
                            md5_all_zero = false;
                            break;
                        }
                    }

                    // Open WAV file and write header
                    wav_file = std::fopen(output_file, "wb");
                    if (!wav_file) {
                        std::fprintf(stderr, "Error: Could not create output file: %s\n",
                                     output_file);
                        std::fclose(flac_file);
                        return 1;
                    }
                    {
                        uint16_t wav_bps =
                            output_32bit ? BIT_DEPTH_32 : static_cast<uint16_t>(bits_per_sample);
                        write_wav_header(wav_file, sample_rate, static_cast<uint16_t>(num_channels),
                                         wav_bps, static_cast<uint32_t>(total_samples));
                    }
                    header_end_pos = std::ftell(wav_file);
                    break;
                }
                case FLAC_DECODER_SUCCESS:
                    process_decoded_samples(samples);
                    break;
                case FLAC_DECODER_NEED_MORE_DATA:
                    // Force inner loop exit: need fresh data from file
                    offset = chunk_len;
                    break;
                case FLAC_DECODER_END_OF_STREAM:
                    done = true;
                    break;
                default:
                    std::fprintf(stderr, "Error: Failed to decode. Error code: %d\n",
                                 static_cast<int>(result));
                    std::fprintf(stderr, "  Frames decoded: %u\n", frames_decoded);
                    if (wav_file) {
                        std::fclose(wav_file);
                    }
                    std::fclose(flac_file);
                    return 1;
            }
        }
    }

    std::printf("Reached end of file.\n");

    std::fclose(flac_file);

    // If total_samples was unknown (0) or incorrect, update the WAV header with actual sample count
    if (wav_file && samples_per_channel_decoded != total_samples &&
        samples_per_channel_decoded > 0) {
        std::fclose(wav_file);
        uint32_t bytes_per_sample = output_32bit ? 4 : (bits_per_sample + 7) / 8;
        update_wav_header(output_file, header_end_pos, samples_per_channel_decoded, num_channels,
                          bytes_per_sample);
    } else if (wav_file) {
        std::fclose(wav_file);
    }

    std::printf("Successfully converted to WAV!\n");
    std::printf("Frames decoded: %u\n", frames_decoded);
    std::printf("Samples per channel decoded: %u\n", samples_per_channel_decoded);
    std::printf("Output file: %s\n", output_file);

    verify_md5(md5_ctx, md5_sig, md5_all_zero);

    return 0;
}
