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

/// @file frame_header.h
/// @brief FLAC frame header parsing (decoder) and writing (encoder)
///
/// Parsing computes the header length, validates CRC-8 and checks the fields
/// against STREAMINFO. Writing chooses each field's code and emits a
/// fixed-blocksize header with its CRC-8.

#pragma once

#include "micro_flac/flac_decoder.h"

#include <cstdint>

namespace micro_flac {

// ============================================================================
// Parsing
// ============================================================================

/// @brief Parsed frame header fields
struct FrameHeaderInfo {
    uint32_t block_size{0};
    uint32_t channel_assignment{0};
    uint32_t bits_per_sample{0};
};

/// @brief Compute the exact frame header length from the first 5 accumulated bytes
///
/// FLAC frame headers are 6-16 bytes. After 5 bytes, the remaining length is deterministic.
/// Layout: sync(2) + block_size/sample_rate(1) + channel/depth(1) + utf8(1-7) +
/// block_size_extra(0-2) + sample_rate_extra(0-2) + crc8(1)
///
/// @param header Pointer to first 5 bytes of the frame header
/// @return Total frame header length in bytes, or 0 if the UTF-8 coded number is invalid
uint8_t compute_frame_header_length(const uint8_t* header);

/// @brief Parse a complete, pre-buffered FLAC frame header
///
/// Parses the frame header bytes, validates CRC8 (if enabled), and checks that
/// the frame parameters match the STREAMINFO metadata. The caller must ensure the
/// header buffer contains exactly the number of bytes returned by
/// compute_frame_header_length().
///
/// @param header Pointer to complete frame header bytes
/// @param header_len Length of the header buffer in bytes
/// @param stream_info Reference to STREAMINFO for validation
/// @param crc_check Whether to validate the CRC8 checksum
/// @param info [out] Parsed frame header fields
/// @return FLAC_DECODER_SUCCESS on success, negative error code on failure
FLACDecoderResult parse_frame_header(const uint8_t* header, uint8_t header_len,
                                     const FLACStreamInfo& stream_info, bool crc_check,
                                     FrameHeaderInfo& info);

// ============================================================================
// Writing
// ============================================================================

/// @brief Largest frame header write_frame_header() produces
///
/// 4 fixed bytes, a 6-byte coded frame number (31 bits), 2 bytes each of
/// uncommon block size and sample rate, and the CRC-8.
static constexpr uint8_t FRAME_HEADER_MAX_WRITE_LENGTH = 15;

/// @brief A sample rate's frame header code and the extra bytes it needs
struct SampleRateCode {
    uint8_t code{0};
    uint8_t extra[2]{};    // Big-endian
    uint8_t extra_len{0};  // 0, 1, or 2
};

/// @brief The fields of a fixed-blocksize frame header
struct FrameHeaderFields {
    uint32_t frame_number{0};
    uint32_t block_size{0};
    uint8_t channel_assignment{0};
    uint8_t bits_per_sample_code{0};
    SampleRateCode sample_rate;
};

/// @brief Choose the frame header code for a sample rate
///
/// Prefers a table entry, then the escape with the fewest extra bytes, then
/// code 0 (read the rate from STREAMINFO), which is outside the streamable
/// subset.
///
/// @param sample_rate Sample rate in Hz
/// @param out [out] The code and its extra bytes
/// @return false if STREAMINFO cannot carry the rate (0, or above 1048575 Hz)
bool select_sample_rate_code(uint32_t sample_rate, SampleRateCode& out);

/// @brief Frame header code for a bit depth
///
/// @return The table's code, or BPS_CODE_FROM_STREAMINFO for a depth the
///         table has no entry for
uint8_t select_bits_per_sample_code(uint32_t bits_per_sample);

/// @brief Write a fixed-blocksize frame header, ending with its CRC-8
///
/// @param out Destination, with room for FRAME_HEADER_MAX_WRITE_LENGTH bytes
/// @param fields The header's fields; frame_number must fit in 31 bits
/// @return Bytes written
uint8_t write_frame_header(uint8_t* out, const FrameHeaderFields& fields);

}  // namespace micro_flac
