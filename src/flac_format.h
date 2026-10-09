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

/// @file flac_format.h
/// @brief FLAC bitstream constants (RFC 9639), shared by the decoder and encoder
///
/// Every code, table, field width and limit the format defines lives here, so
/// the two directions cannot disagree on one. The exceptions: the CRC lookup
/// tables, derived from the format's polynomials, live in crc.cpp, and the
/// public headers carry the few values their API exposes (e.g. the metadata
/// block types, the block size limits, STREAMINFO's size as
/// FLACStreamInfo::RAW_SIZE).

#pragma once

#include <cstddef>
#include <cstdint>

namespace micro_flac {

// ============================================================================
// Stream (RFC 9639 Section 8)
// ============================================================================

/// @brief Stream marker that begins every native FLAC stream
static constexpr uint8_t MAGIC_BYTES[] = {'f', 'L', 'a', 'C'};

/// @brief STREAMINFO metadata block body length in bytes
static constexpr size_t STREAMINFO_SIZE = 34;

/// @brief Shallowest bit depth a stream can have (Section 8.2)
static constexpr uint32_t MIN_BITS_PER_SAMPLE = 4;

/// @brief Largest value of STREAMINFO's 36-bit total-samples field
static constexpr uint64_t TOTAL_SAMPLES_MAX = (1ULL << 36) - 1;

/// @brief Largest value of STREAMINFO's 24-bit minimum/maximum frame-size fields
static constexpr uint32_t FRAME_BYTES_MAX = (1UL << 24) - 1;

// ============================================================================
// Frame header (RFC 9639 Section 9.1)
// ============================================================================

/// @brief Block size lookup table (Section 9.1.1)
///
/// Indexed by the 4-bit block size code. Codes 6 and 7 read the size from an
/// 8/16-bit extra field and are 0 here, as is the reserved code 0.
static constexpr uint16_t BLOCK_SIZE_TABLE[16] = {0,   192, 576,  1152, 2304, 4608, 0,     0,
                                                  256, 512, 1024, 2048, 4096, 8192, 16384, 32768};

/// @brief Block size codes reading an 8- or 16-bit "block size - 1" extra
/// field (Section 9.1.6)
static constexpr uint8_t BLOCK_SIZE_CODE_8BIT = 6;
static constexpr uint8_t BLOCK_SIZE_CODE_16BIT = 7;

/// @brief Sample rate lookup table (Section 9.1.2)
///
/// Indexed by (code - 1) for codes 1-11. Code 0 means "from STREAMINFO",
/// codes 12-14 read the rate from an extra field, and code 15 is invalid.
static constexpr uint32_t SAMPLE_RATE_TABLE[11] = {88200, 176400, 192000, 8000,  16000, 22050,
                                                   24000, 32000,  44100,  48000, 96000};

/// @brief Highest sample-rate code covered by SAMPLE_RATE_TABLE
static constexpr uint8_t SAMPLE_RATE_TABLE_MAX = 11;
/// @brief Sample rate code: read the rate from STREAMINFO
static constexpr uint8_t SAMPLE_RATE_CODE_FROM_STREAMINFO = 0;
/// @brief Sample rate code: kHz in a 1-byte extra field
static constexpr uint8_t SAMPLE_RATE_CODE_KHZ_1BYTE = 12;
/// @brief Sample rate code: Hz in a 2-byte extra field
static constexpr uint8_t SAMPLE_RATE_CODE_HZ_2BYTE = 13;
/// @brief Sample rate code: tens of Hz in a 2-byte extra field
static constexpr uint8_t SAMPLE_RATE_CODE_TENS_HZ_2BYTE = 14;

/// @brief Largest rate each sample-rate escape can carry (Section 9.1.2)
static constexpr uint32_t SAMPLE_RATE_MAX_KHZ_1BYTE = 255000;
static constexpr uint32_t SAMPLE_RATE_MAX_HZ_2BYTE = 65535;
static constexpr uint32_t SAMPLE_RATE_MAX_TENS_HZ_2BYTE = 655350;

/// @brief Largest rate STREAMINFO's 20-bit field can carry (Section 8.2)
static constexpr uint32_t SAMPLE_RATE_MAX = 0xFFFFF;

/// @brief Largest number of channels a stream can carry (Section 9.1.3)
static constexpr uint32_t MAX_CHANNELS = 8;

/// @brief Two channels coded independently: code 1 of the 0-7 range, which
/// means that many channels plus one (Section 9.1.3)
static constexpr uint32_t CHANNEL_INDEPENDENT_STEREO = 1;

/// @brief Joint stereo channel assignment codes (Section 9.1.3)
static constexpr uint32_t CHANNEL_LEFT_SIDE = 8;
static constexpr uint32_t CHANNEL_RIGHT_SIDE = 9;
static constexpr uint32_t CHANNEL_MID_SIDE = 10;

/// @brief Bits-per-sample lookup table (Section 9.1.4)
///
/// Indexed by the 3-bit bit depth code. A 0 entry marks a reserved code,
/// except code 0 itself (BPS_CODE_FROM_STREAMINFO).
static constexpr uint8_t BPS_TABLE[8] = {0, 8, 12, 0, 16, 20, 24, 32};

/// @brief Bit depth code meaning "read the depth from STREAMINFO"
static constexpr uint8_t BPS_CODE_FROM_STREAMINFO = 0;

/// @brief Largest coded frame number of a fixed-blocksize stream (31 bits, Section 9.1.5)
static constexpr uint32_t FRAME_NUMBER_MAX = (1UL << 31) - 1;

/// @brief Shortest valid frame header: sync code, reserved and
/// blocking-strategy bits, block size and rate codes, channel and depth
/// codes, a 1-byte coded number, and the CRC-8
static constexpr uint8_t FRAME_HEADER_MIN_LENGTH = 6;

// ============================================================================
// Subframes (RFC 9639 Section 9.2)
// ============================================================================

/// @brief Subframe type codes (Section 9.2.1)
///
/// FIXED is FIXED_MIN + order (orders 0-4); LPC is LPC_MIN + order - 1
/// (orders 1-32).
static constexpr uint8_t SUBFRAME_TYPE_CONSTANT = 0;
static constexpr uint8_t SUBFRAME_TYPE_VERBATIM = 1;
static constexpr uint8_t SUBFRAME_TYPE_FIXED_MIN = 8;
static constexpr uint8_t SUBFRAME_TYPE_FIXED_MAX = 12;
static constexpr uint8_t SUBFRAME_TYPE_LPC_MIN = 32;
static constexpr uint8_t SUBFRAME_TYPE_LPC_MAX = 63;

/// @brief Bits in a subframe header before any wasted-bits count (Section 9.2.1):
/// a zero pad bit, the 6-bit type and the wasted-bits flag
static constexpr uint8_t SUBFRAME_HEADER_BITS = 8;

/// @brief Highest fixed-predictor order (Section 9.2.5)
static constexpr uint8_t MAX_FIXED_ORDER = 4;

/// @brief Fixed-predictor coefficients (Section 9.2.5), oldest sample first
///
/// Indexed by order. Order 0 has no coefficients and is nullptr.
static constexpr int16_t FIXED_COEFFICIENTS_1[] = {1};
static constexpr int16_t FIXED_COEFFICIENTS_2[] = {-1, 2};
static constexpr int16_t FIXED_COEFFICIENTS_3[] = {1, -3, 3};
static constexpr int16_t FIXED_COEFFICIENTS_4[] = {-1, 4, -6, 4};
static constexpr const int16_t* FIXED_COEFFICIENTS[] = {nullptr, FIXED_COEFFICIENTS_1,
                                                        FIXED_COEFFICIENTS_2, FIXED_COEFFICIENTS_3,
                                                        FIXED_COEFFICIENTS_4};

/// @brief Bits in a residual's coding method field (Section 9.2.7)
static constexpr uint8_t RESIDUAL_CODING_METHOD_BITS = 2;

/// @brief Bits in a Rice parameter under coding method 0 (Section 9.2.7)
///
/// Method 1 uses one more.
static constexpr uint32_t RICE_PARAMETER_BITS = 4;

/// @brief Largest 4-bit Rice parameter; 15 is that method's escape code
static constexpr uint8_t RICE_PARAMETER_MAX_4BIT = 14;

/// @brief Largest 5-bit Rice parameter; 31 is that method's escape code
static constexpr uint8_t RICE_PARAMETER_MAX = 30;

/// @brief Bits in a residual's partition order field (Section 9.2.7)
static constexpr uint8_t RICE_PARTITION_ORDER_BITS = 4;

// ============================================================================
// Ogg encapsulation (RFC 9639 Section 10.1)
// ============================================================================

/// @brief Fixed start of the Ogg FLAC first packet: 0x7F, "FLAC", major version 1
static constexpr uint8_t OGG_BOS_PREFIX[] = {0x7F, 'F', 'L', 'A', 'C', 0x01};

/// @brief Bytes before "fLaC" in the first packet: the prefix, the minor
/// version and the 2-byte header packet count
static constexpr uint8_t OGG_BOS_HEADER_LENGTH = 9;

}  // namespace micro_flac
