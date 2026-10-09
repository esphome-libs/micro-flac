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
#include "flac_format.h"

#include <cstddef>

namespace micro_flac {

namespace {

// ============================================================================
// Parsing
// ============================================================================

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
    if (block_size_code == BLOCK_SIZE_CODE_8BIT) {
        layout.block_size_extra = 1;
    } else if (block_size_code == BLOCK_SIZE_CODE_16BIT) {
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
    uint8_t block_size_code = header[2] >> 4;
    if (block_size_code == 0) {
        return FLAC_DECODER_ERROR_BAD_BLOCK_SIZE;
    }
    if (block_size_code != BLOCK_SIZE_CODE_8BIT && block_size_code != BLOCK_SIZE_CODE_16BIT) {
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
    if (block_size_code == BLOCK_SIZE_CODE_8BIT) {
        info.block_size = header[extra_idx] + 1;
        extra_idx += 1;
    } else if (block_size_code == BLOCK_SIZE_CODE_16BIT) {
        info.block_size = (static_cast<uint32_t>(header[extra_idx]) << 8) | header[extra_idx + 1];
        info.block_size += 1;
        extra_idx += 2;
    }

    // 9.1.7 Uncommon sample rate
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

// ============================================================================
// Writing
// ============================================================================

namespace {

// The block size's code, plus its extra field for a size the table lacks
uint8_t select_block_size_code(uint32_t n, uint8_t (&extra)[2], uint8_t& extra_len) {
    for (size_t i = 1; i < sizeof(BLOCK_SIZE_TABLE) / sizeof(BLOCK_SIZE_TABLE[0]); i++) {
        if (i != BLOCK_SIZE_CODE_8BIT && i != BLOCK_SIZE_CODE_16BIT && BLOCK_SIZE_TABLE[i] == n) {
            extra_len = 0;
            return static_cast<uint8_t>(i);
        }
    }
    const uint32_t v = n - 1;
    if (v <= 0xFFU) {
        extra[0] = static_cast<uint8_t>(v);
        extra_len = 1;
        return BLOCK_SIZE_CODE_8BIT;
    }
    extra[0] = static_cast<uint8_t>((v >> 8) & 0xFFU);
    extra[1] = static_cast<uint8_t>(v & 0xFFU);
    extra_len = 2;
    return BLOCK_SIZE_CODE_16BIT;
}

// UTF-8-style coded number (RFC 9639 Section 9.1.5), up to 6 bytes (31 bits)
uint8_t write_coded_number(uint8_t* out, uint32_t value) {
    if (value < 0x80U) {
        out[0] = static_cast<uint8_t>(value);
        return 1;
    }
    uint8_t num_bytes = 2;
    while (num_bytes < 6 && value >= (1UL << (5 * num_bytes + 1))) {
        num_bytes++;
    }
    uint32_t remaining = value;
    for (uint8_t i = num_bytes; i-- > 1;) {
        out[i] =
            static_cast<uint8_t>(0x80U | (remaining & 0x3FU));  // NOLINT(readability-magic-numbers)
        remaining >>= 6;
    }
    // num_bytes leading ones, a zero, then the value's top bits
    out[0] = static_cast<uint8_t>((0xFFU << (8U - num_bytes)) | remaining);
    return num_bytes;
}

}  // namespace

bool select_sample_rate_code(uint32_t sample_rate, SampleRateCode& out) {
    out = SampleRateCode{};
    if (sample_rate == 0 || sample_rate > SAMPLE_RATE_MAX) {
        return false;
    }
    for (uint8_t i = 0; i < SAMPLE_RATE_TABLE_MAX; i++) {
        if (SAMPLE_RATE_TABLE[i] == sample_rate) {
            out.code = static_cast<uint8_t>(i + 1);
            return true;
        }
    }
    // NOLINTBEGIN(readability-magic-numbers)
    if (sample_rate % 1000 == 0 && sample_rate <= SAMPLE_RATE_MAX_KHZ_1BYTE) {
        out.code = SAMPLE_RATE_CODE_KHZ_1BYTE;
        out.extra[0] = static_cast<uint8_t>(sample_rate / 1000);
        out.extra_len = 1;
        return true;
    }
    uint32_t field = 0;
    if (sample_rate <= SAMPLE_RATE_MAX_HZ_2BYTE) {
        out.code = SAMPLE_RATE_CODE_HZ_2BYTE;
        field = sample_rate;
    } else if (sample_rate % 10 == 0 && sample_rate <= SAMPLE_RATE_MAX_TENS_HZ_2BYTE) {
        out.code = SAMPLE_RATE_CODE_TENS_HZ_2BYTE;
        field = sample_rate / 10;
    } else {
        out.code = SAMPLE_RATE_CODE_FROM_STREAMINFO;
        return true;
    }
    // NOLINTEND(readability-magic-numbers)
    out.extra[0] = static_cast<uint8_t>((field >> 8) & 0xFFU);
    out.extra[1] = static_cast<uint8_t>(field & 0xFFU);
    out.extra_len = 2;
    return true;
}

uint8_t select_bits_per_sample_code(uint32_t bits_per_sample) {
    for (size_t i = 1; i < sizeof(BPS_TABLE); i++) {
        if (BPS_TABLE[i] == bits_per_sample) {
            return static_cast<uint8_t>(i);
        }
    }
    return BPS_CODE_FROM_STREAMINFO;
}

uint8_t write_frame_header(uint8_t* out, const FrameHeaderFields& fields) {
    uint8_t block_size_extra[2] = {0, 0};
    uint8_t block_size_extra_len = 0;
    const uint8_t block_size_code =
        select_block_size_code(fields.block_size, block_size_extra, block_size_extra_len);

    out[0] = 0xFF;  // Sync code
    out[1] = 0xF8;  // Sync code, reserved bit, fixed blocksize  NOLINT(readability-magic-numbers)
    out[2] = static_cast<uint8_t>((block_size_code << 4) | fields.sample_rate.code);
    out[3] =
        static_cast<uint8_t>((fields.channel_assignment << 4) | (fields.bits_per_sample_code << 1));
    uint8_t len = static_cast<uint8_t>(4 + write_coded_number(out + 4, fields.frame_number));
    for (uint8_t i = 0; i < block_size_extra_len; i++) {
        out[len++] = block_size_extra[i];
    }
    for (uint8_t i = 0; i < fields.sample_rate.extra_len; i++) {
        out[len++] = fields.sample_rate.extra[i];
    }
    out[len] = calculate_crc8(out, len);
    return static_cast<uint8_t>(len + 1);
}

}  // namespace micro_flac
