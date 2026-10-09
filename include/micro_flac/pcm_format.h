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

/// @file pcm_format.h
/// @brief Description of a packed, interleaved PCM byte stream

#pragma once

#include <cstdint>

namespace micro_flac {

// ============================================================================
// PcmFormat
// ============================================================================

/// @brief Format of a packed, interleaved PCM byte stream
///
/// Describes PCM laid out exactly as FLACDecoder's uint8_t* decode() overload
/// produces it, and as FLACEncoder consumes it:
///
/// - Samples are interleaved by channel (L R L R ... for stereo).
/// - Each sample occupies bytes_per_sample() = ceil(bits_per_sample() / 8)
///   bytes, little-endian, two's-complement signed (8-bit samples are signed
///   too, unlike 8-bit WAV data).
/// - Depths that are not a multiple of 8 are left-justified within those
///   bytes: a 12-bit sample sits in the top 12 bits of its 2 bytes, and the
///   low 4 bits are zero on decode and ignored on encode.
///
/// Construct one to describe FLACEncoder's input:
/// @code
/// PcmFormat format{44100, 2, 16};  // sample_rate, num_channels, bits_per_sample
/// @endcode
class PcmFormat {
    // 32-bit fields
    uint32_t sample_rate_{0};  // Sample rate in Hz

    // 8-bit fields
    uint8_t num_channels_{0};     // Interleaved channel count
    uint8_t bits_per_sample_{0};  // Significant bits per sample

public:
    /// @brief An unpopulated format; is_valid() is false
    PcmFormat() = default;

    /// @brief Describe a PCM byte stream
    ///
    /// A channel count or depth above 255 is stored as 0, which FLACEncoder
    /// rejects, rather than wrapping to a value it might accept.
    ///
    /// @param sample_rate Sample rate in Hz
    /// @param num_channels Interleaved channel count
    /// @param bits_per_sample Significant bits per sample
    PcmFormat(uint32_t sample_rate, uint32_t num_channels, uint32_t bits_per_sample)
        : sample_rate_(sample_rate),
          num_channels_(num_channels <= UINT8_MAX ? static_cast<uint8_t>(num_channels) : 0),
          bits_per_sample_(bits_per_sample <= UINT8_MAX ? static_cast<uint8_t>(bits_per_sample)
                                                        : 0) {}

    /// @brief Significant bits per sample
    /// @return Bit depth in bits
    uint32_t bits_per_sample() const {
        return this->bits_per_sample_;
    }
    /// @brief Bytes each sample occupies in the byte stream
    /// @return ceil(bits_per_sample() / 8)
    uint32_t bytes_per_sample() const {
        return (static_cast<uint32_t>(this->bits_per_sample_) + 7U) / 8U;
    }
    /// @brief Bytes one sample frame (one sample per channel) occupies
    /// @return bytes_per_sample() * num_channels()
    uint32_t bytes_per_frame() const {
        return this->bytes_per_sample() * this->num_channels_;
    }
    /// @brief Number of interleaved channels (1 = mono, 2 = stereo, etc.)
    /// @return Channel count, or 0 if unpopulated
    uint32_t num_channels() const {
        return this->num_channels_;
    }
    /// @brief Sample rate in Hz (e.g., 44100, 48000)
    /// @return Sample rate in Hz, or 0 if unpopulated
    uint32_t sample_rate() const {
        return this->sample_rate_;
    }
    /// @brief Whether the format has been populated
    ///
    /// Only whether a sample rate is set: FLACEncoder validates the whole
    /// format and reports FLAC_ENCODER_ERROR_BAD_CONFIG from its first call.
    ///
    /// @return true once a sample rate is set
    bool is_valid() const {
        return this->sample_rate_ != 0;
    }
};

}  // namespace micro_flac
