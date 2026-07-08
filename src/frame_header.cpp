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

#include "frame_header.h"

#include "crc.h"
#include "decorrelation.h"

namespace micro_flac {

// FLAC sample rate codes that require extra header bytes (RFC 9639 Section 9.1.2)
static constexpr uint8_t SAMPLE_RATE_TABLE_MAX = 11;
static constexpr uint8_t SAMPLE_RATE_CODE_KHZ_1BYTE = 12;
static constexpr uint8_t SAMPLE_RATE_CODE_HZ_2BYTE = 13;
static constexpr uint8_t SAMPLE_RATE_CODE_TENS_HZ_2BYTE = 14;

namespace {

// Layout of the variable-position frame header fields (RFC 9639 Section 9.1):
// [sync+flags(4) | coded number(1-7) | block size extra(0-2) |
// sample rate extra(0-2) | crc8(1)], all determined by the first five bytes.
// Single source of truth for compute_frame_header_length and
// parse_frame_header so their field positions can never disagree.
struct FrameHeaderLayout {
    uint8_t utf8_len;           // coded number byte count; 0 = invalid encoding
    uint8_t block_size_extra;   // uncommon block size bytes (0-2)
    uint8_t sample_rate_extra;  // uncommon sample rate bytes (0-2)
};

FrameHeaderLayout compute_layout(const uint8_t* header) {
    FrameHeaderLayout layout{};

    // UTF-8 coded number length from its leading byte (byte[4])
    uint8_t utf8_first = header[4];
    layout.utf8_len = 1;
    // Continuation bytes (0x80-0xBF) and 0xFF are invalid as a leading byte;
    // RFC 9639 Table 18 tops out at 0xFE.
    // NOLINTNEXTLINE(readability-magic-numbers)
    if ((utf8_first >= 0x80 && utf8_first < 0xC0) || utf8_first == 0xFF) {
        layout.utf8_len = 0;
    } else if (utf8_first >= 0xC0) {  // NOLINT(readability-magic-numbers)
        uint8_t mask = utf8_first;
        layout.utf8_len = 0;
        while (mask & 0x80) {
            layout.utf8_len++;
            mask = static_cast<uint8_t>(mask << 1);
        }
        // The 7-byte form (0xFE) encodes 36 bits, which only sample numbers use.
        // Fixed-block-size streams (blocking strategy bit clear) carry a frame
        // number capped at 31 bits = 6 encoded bytes (RFC 9639 Section 9.1.5).
        if (layout.utf8_len == 7 && (header[1] & 0x01) == 0) {
            layout.utf8_len = 0;
        }
    }

    // Block size extra bytes from block_size_code (upper nibble of byte[2])
    uint8_t block_size_code = header[2] >> 4;
    if (block_size_code == 6) {
        layout.block_size_extra = 1;
    } else if (block_size_code == 7) {
        layout.block_size_extra = 2;
    }

    // Sample rate extra bytes from sample_rate_code (lower nibble of byte[2])
    uint8_t sample_rate_code = header[2] & 0x0F;  // NOLINT(readability-magic-numbers)
    if (sample_rate_code == SAMPLE_RATE_CODE_KHZ_1BYTE) {
        layout.sample_rate_extra = 1;
    } else if (sample_rate_code == SAMPLE_RATE_CODE_HZ_2BYTE ||
               sample_rate_code == SAMPLE_RATE_CODE_TENS_HZ_2BYTE) {
        layout.sample_rate_extra = 2;
    }

    return layout;
}

uint8_t layout_total_length(const FrameHeaderLayout& layout) {
    // Total: 4 fixed bytes + coded number + extras + 1 CRC-8
    return static_cast<uint8_t>(4 + layout.utf8_len + layout.block_size_extra +
                                layout.sample_rate_extra + 1);
}

}  // namespace

uint8_t compute_frame_header_length(const uint8_t* header) {
    FrameHeaderLayout layout = compute_layout(header);
    if (layout.utf8_len == 0) {
        return 0;
    }
    return layout_total_length(layout);
}

FLACDecoderResult parse_frame_header(const uint8_t* header, uint8_t header_len,
                                     const FLACStreamInfo& stream_info, bool crc_check,
                                     FrameHeaderInfo& info) {
    // Bytes 0-1: sync code + reserved bit (already validated by
    // decode_frame_header_phase via the 0xFE mask, which forces header[1] bit 1 = 0)

    // Byte 2: block_size_code (upper nibble) + sample_rate_code (lower nibble)
    if (header[2] == 0xFF) {
        // Sync code cannot appear in the header; erroneous sync
        return FLAC_DECODER_ERROR_SYNC_NOT_FOUND;
    }

    // 9.1.1 Block size bits
    static constexpr uint16_t BLOCK_SIZE_TABLE[] = {0,   192, 576,  1152, 2304, 4608, 0,     0,
                                                    256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
    uint8_t block_size_code = header[2] >> 4;
    if (block_size_code == 0) {
        return FLAC_DECODER_ERROR_BAD_BLOCK_SIZE;
    }
    if (block_size_code != 6 && block_size_code != 7) {
        info.block_size = BLOCK_SIZE_TABLE[block_size_code];
    }

    // 9.1.2 Sample rate bits
    uint8_t sample_rate_code = header[2] & 0x0F;  // NOLINT(readability-magic-numbers)

    // Byte 3: channel_assign (upper nibble) + bps_code (bits 3-1) + reserved (bit 0)
    if (header[3] == 0xFF) {
        return FLAC_DECODER_ERROR_SYNC_NOT_FOUND;
    }

    // 9.1.3 Channel bits
    info.channel_assignment = header[3] >> 4;

    // 9.1.4 Bit depth bits
    static constexpr uint8_t BPS_TABLE[] = {0, 8, 12, 0, 16, 20, 24, 32};
    uint8_t bits_per_sample_code = (header[3] & 0x0E) >> 1;  // NOLINT(readability-magic-numbers)
    if (bits_per_sample_code == 0) {
        info.bits_per_sample = stream_info.bits_per_sample();
    } else if (BPS_TABLE[bits_per_sample_code] == 0) {
        return FLAC_DECODER_ERROR_BAD_SAMPLE_DEPTH;
    } else {
        info.bits_per_sample = BPS_TABLE[bits_per_sample_code];
    }

    // Reserved bit (header[3] & 0x01) not checked; some encoders don't respect it

    // 9.1.5 Coded number (UTF-8 like variable length code) - value skipped, seeking
    // not supported. The extra fields' positions come from the shared layout;
    // header_len was derived from the same layout by compute_frame_header_length,
    // so a mismatch means the caller handed us a differently-sized buffer.
    FrameHeaderLayout layout = compute_layout(header);
    if (layout.utf8_len == 0 || layout_total_length(layout) != header_len) {
        return FLAC_DECODER_ERROR_BAD_HEADER;
    }

    // Index of the extra fields, just past the coded number
    uint8_t extra_idx = static_cast<uint8_t>(4 + layout.utf8_len);

    // The coded number's value is unused (no seeking), but its continuation bytes
    // (header[5] through the byte before the extra fields) must still match the
    // 10xxxxxx pattern (RFC 9639 Table 18) for the header to be well formed.
    for (uint8_t i = 5; i < extra_idx; i++) {
        if ((header[i] & 0xC0) != 0x80) {  // NOLINT(readability-magic-numbers)
            return FLAC_DECODER_ERROR_BAD_HEADER;
        }
    }

    // 9.1.6 Uncommon block size
    if (block_size_code == 6) {
        info.block_size = header[extra_idx] + 1;
        extra_idx += 1;
    } else if (block_size_code == 7) {
        info.block_size = (static_cast<uint32_t>(header[extra_idx]) << 8) | header[extra_idx + 1];
        info.block_size += 1;
        extra_idx += 2;
    }

    // 9.1.7 Uncommon sample rate
    static constexpr uint32_t SAMPLE_RATE_TABLE[] = {88200, 176400, 192000, 8000,  16000, 22050,
                                                     24000, 32000,  44100,  48000, 96000};
    uint32_t frame_sample_rate = 0;
    if (sample_rate_code >= 1 && sample_rate_code <= SAMPLE_RATE_TABLE_MAX) {
        frame_sample_rate = SAMPLE_RATE_TABLE[sample_rate_code - 1];
    } else if (sample_rate_code == 0) {
        frame_sample_rate = stream_info.sample_rate();
    } else if (sample_rate_code == SAMPLE_RATE_CODE_KHZ_1BYTE) {
        // NOLINTNEXTLINE(readability-magic-numbers)
        frame_sample_rate = header[extra_idx] * 1000;
    } else if (sample_rate_code == SAMPLE_RATE_CODE_HZ_2BYTE) {
        frame_sample_rate = (static_cast<uint32_t>(header[extra_idx]) << 8) | header[extra_idx + 1];
    } else if (sample_rate_code == SAMPLE_RATE_CODE_TENS_HZ_2BYTE) {
        frame_sample_rate =
            // NOLINTNEXTLINE(readability-magic-numbers)
            ((static_cast<uint32_t>(header[extra_idx]) << 8) | header[extra_idx + 1]) * 10;
    } else {
        // sample_rate_code == 15 is invalid/reserved
        return FLAC_DECODER_ERROR_BAD_SAMPLE_RATE;
    }

    // 9.1.8 Frame header CRC
    if (crc_check) {
        uint8_t crc_read = header[header_len - 1];
        uint8_t crc_calculated = calculate_crc8(header, header_len - 1);
        if (crc_calculated != crc_read) {
            return FLAC_DECODER_ERROR_CRC_MISMATCH;
        }
    }

    uint32_t frame_channels = 0;
    if (info.channel_assignment <= 7) {
        frame_channels = info.channel_assignment + 1;
    } else if (info.channel_assignment <= CHANNEL_MID_SIDE) {
        frame_channels = 2;  // Stereo decorrelation modes
    } else {
        // Channel assignments 11-15 are reserved per RFC 9639
        return FLAC_DECODER_ERROR_RESERVED_CHANNEL_ASSIGNMENT;
    }

    if (frame_channels != stream_info.num_channels()) {
        return FLAC_DECODER_ERROR_FRAME_MISMATCH;
    }

    // Validate that frame bit depth matches STREAMINFO (when frame specifies bit depth)
    if (bits_per_sample_code != 0 && info.bits_per_sample != stream_info.bits_per_sample()) {
        return FLAC_DECODER_ERROR_FRAME_MISMATCH;
    }

    if (frame_sample_rate != stream_info.sample_rate()) {
        return FLAC_DECODER_ERROR_FRAME_MISMATCH;
    }

    return FLAC_DECODER_SUCCESS;
}

}  // namespace micro_flac
