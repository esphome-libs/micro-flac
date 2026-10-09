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

/// @file pcm_packing.h
/// @brief Interleaved PCM packing (decoder output) and unpacking (encoder input)
///
/// Packing converts planar decoded samples to interleaved PCM, with fast paths
/// for common formats (16-bit stereo/mono, 24-bit stereo, 32-bit output).
/// Unpacking reads the encoder's packed input (PcmFormat's layout) into
/// int32_t samples.

#pragma once

#include <cstdint>

namespace micro_flac {

// ============================================================================
// Packing
// ============================================================================

/// @brief Write decoded block samples to an interleaved PCM output buffer
///
/// Dispatches to optimized fast paths for common formats (16-bit stereo/mono,
/// 24-bit stereo, 32-bit stereo/mono) and falls back to a general path for
/// other configurations.
///
/// @param output_buffer    Destination for interleaved PCM samples
/// @param block_samples    Decoded samples in planar layout [ch0...ch0, ch1...ch1, ...]
/// @param block_size       Number of samples per channel in this block
/// @param bits_per_sample     Bit depth of the decoded samples (e.g. 16, 24)
/// @param num_channels     Number of audio channels
/// @param output_32bit     If true, output all samples as 32-bit left-justified values
void write_samples(uint8_t* output_buffer, const int32_t* block_samples, uint32_t block_size,
                   uint32_t bits_per_sample, uint32_t num_channels, bool output_32bit);

// ============================================================================
// Unpacking
// ============================================================================

/// @brief Deinterleave one channel of packed PCM into int32_t samples
///
/// The input is PcmFormat's layout: interleaved, little-endian, `bytes` per
/// sample, left-justified, with 2-byte samples 2-byte aligned.
///
/// @param dst          Destination for n right-justified, sign-extended samples
/// @param src          First frame of packed input
/// @param n            Number of frames to read
/// @param ch           Channel to read
/// @param num_channels Channels per frame
/// @param bps          Bits to keep per sample; less than the stream's depth
///                     drops that many low bits
/// @param bytes        Bytes per packed sample (1, 2 or 3)
void unpack_channel(int32_t* dst, const uint8_t* src, uint32_t n, uint32_t ch,
                    uint32_t num_channels, uint32_t bps, uint32_t bytes);

/// @brief Deinterleave packed stereo into left and right arrays
///
/// Parameters as unpack_channel().
void unpack_stereo(int32_t* left, int32_t* right, const uint8_t* src, uint32_t n, uint32_t bps,
                   uint32_t bytes);

/// @brief Derive the mid channel of packed stereo, (left + right) >> (1 + wasted)
///
/// Reads the input once, without storing left and right first.
///
/// @param dst    Destination for n samples
/// @param src    First frame of packed stereo input
/// @param n      Number of frames to read
/// @param bps    The stream's depth, which left and right are read at
/// @param bytes  Bytes per packed sample (1, 2 or 3)
/// @param wasted Low zero bits shifted out of every derived sample
void unpack_mid(int32_t* dst, const uint8_t* src, uint32_t n, uint32_t bps, uint32_t bytes,
                uint32_t wasted);

/// @brief Derive the side channel of packed stereo, (left - right) >> wasted
///
/// Parameters as unpack_mid().
void unpack_side(int32_t* dst, const uint8_t* src, uint32_t n, uint32_t bps, uint32_t bytes,
                 uint32_t wasted);

/// @brief Read one packed sample
///
/// Parameters as unpack_channel().
int32_t unpack_sample(const uint8_t* p, uint32_t bps, uint32_t bytes);

}  // namespace micro_flac
