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

/// @file flac_encoder.h
/// @brief FLAC encoder optimized for ESP32
///
/// Encodes 4-24 bit PCM in 1-8 channels from the packed byte layout in
/// pcm_format.h, which is what FLACDecoder's uint8_t* decode() produces.
/// Spec: https://www.rfc-editor.org/rfc/rfc9639.html

#pragma once

#include "micro_flac/pcm_format.h"

#include <cstddef>
#include <cstdint>

// Marks functions whose return value must not be ignored (errors are reported
// only through return codes). [[nodiscard]] needs C++17; the library builds
// at C++14, so fall back to the GNU attribute there. flac_decoder.h and
// flac_encoder.h each define it, so either can be included alone.
#ifndef MICRO_FLAC_NODISCARD
#if defined(__cplusplus) && __cplusplus >= 201703L
#define MICRO_FLAC_NODISCARD [[nodiscard]]
#elif defined(__GNUC__)
#define MICRO_FLAC_NODISCARD __attribute__((warn_unused_result))
#else
#define MICRO_FLAC_NODISCARD
#endif
#endif

namespace micro_flac {

// ============================================================================
// Public Types
// ============================================================================

/// @brief Result codes returned by FLACEncoder methods
///
/// Negative values are errors. An error consumes no input, reports no output
/// (bytes_written is 0; the output buffer's contents are unspecified), and
/// leaves the stream where it was:
///
/// - FLAC_ENCODER_ERROR_OUTPUT_BUFFER_TOO_SMALL and
///   FLAC_ENCODER_ERROR_INVALID_ARGUMENT are recoverable: fix the call and retry it.
/// - FLAC_ENCODER_ERROR_BAD_CONFIG persists until reset(format, options)
///   supplies a supported configuration.
/// - FLAC_ENCODER_ERROR_STREAM_FINISHED means finish() already ended the
///   stream; call reset() to start a new one.
/// - FLAC_ENCODER_ERROR_STREAM_TOO_LONG means no further frame fits the
///   format's frame numbering. finish() with no input still ends the stream
///   cleanly; then reset() to start a new one.
enum FLACEncoderResult : int8_t {
    // Success / informational (>= 0)
    FLAC_ENCODER_SUCCESS = 0,         // The call did its work (check bytes_written)
    FLAC_ENCODER_NEED_MORE_DATA = 1,  // encode() was given less than one block; nothing consumed

    // Errors (< 0)
    FLAC_ENCODER_ERROR_OUTPUT_BUFFER_TOO_SMALL = -1,  // Output buffer too small for the call
    FLAC_ENCODER_ERROR_INVALID_ARGUMENT = -2,  // Null pointer, misaligned 2-byte-sample input,
                                               // or a finish() remainder that is not whole
                                               // sample frames or is longer than one block
    FLAC_ENCODER_ERROR_BAD_CONFIG = -3,        // Unsupported PcmFormat or FLACEncoderOptions
    FLAC_ENCODER_ERROR_STREAM_FINISHED = -4,   // encode()/finish() after finish(); call reset()
    FLAC_ENCODER_ERROR_STREAM_TOO_LONG = -5    // Next frame number would exceed 2^31 - 1
};

/// @brief Compression settings, fixed at construction
///
/// Every field's default works for any format.
struct FLACEncoderOptions {
    /// Samples per channel in every frame but the last, in
    /// [FLACEncoder::MIN_BLOCK_SIZE, FLACEncoder::MAX_BLOCK_SIZE].
    uint32_t block_size{4096};

    /// Largest Rice partition order tried, in [0,
    /// FLACEncoder::MAX_RICE_PARTITION_ORDER]; 0 codes each subframe with one
    /// Rice parameter. Partitioning (RFC 9639 SS9.2.7) gives each of up to
    /// 2^order stretches of a subframe its own parameter. It pays off mostly
    /// at large blocks: order 6 makes music about 1% smaller at 4096 samples,
    /// for about 19% more encode time on an ESP32-S3.
    uint8_t max_rice_partition_order{0};

    /// Code a subframe whose samples all end in the same k zero bits as
    /// (depth - k)-bit samples (RFC 9639 SS9.2.2), as with 16-bit audio in a
    /// 24-bit stream. Costs 1-3% on an ESP32-S3.
    bool wasted_bits{true};
};

// ============================================================================
// FLACEncoder
// ============================================================================

/**
 * @brief FLAC audio encoder optimized for ESP32
 *
 * Encodes packed PCM (see PcmFormat) to a native, fixed-blocksize FLAC
 * stream, one block per encode() call. Each subframe uses the cheapest fixed
 * predictor (orders 0-4).
 * Stereo frames pick the cheapest of the four channel assignments.
 *
 * The encoder never buffers input: encode() consumes exactly one block
 * (get_input_block_bytes()) from the caller's buffer, or nothing and returns
 * FLAC_ENCODER_NEED_MORE_DATA. finish() encodes the remainder as a final,
 * possibly short, frame.
 *
 * @warning Thread Safety: This class is NOT thread-safe. Each encoder instance
 *          must be accessed from only one thread at a time. To encode
 *          multiple streams concurrently, create a separate encoder for each
 *          thread.
 *
 * @note The encoder never allocates; its working memory is the object itself
 *       (about 2.1 KB), plus up to about 2.3 KB of task stack inside
 *       encode() and finish(). The constructor always succeeds, and an
 *       unsupported configuration is reported by the first write_header(),
 *       encode() or finish() call as FLAC_ENCODER_ERROR_BAD_CONFIG. To
 *       change the format or options, call reset(format, options) rather
 *       than constructing a new encoder.
 *
 * @note Input is unpacked into a buffer inside the object a chunk at a time,
 *       and every format takes the same path.
 *
 * Usage:
 * 1. Construct with the input's PcmFormat and, optionally, FLACEncoderOptions
 * 2. Call write_header() and write its HEADER_BYTES bytes out
 * 3. Call encode() in a loop, advancing the input by bytes_consumed, until it
 *    returns FLAC_ENCODER_NEED_MORE_DATA; append more input and repeat
 * 4. At end of input, call finish() with the remainder
 * 5. Optionally, call write_header() again and overwrite the first
 *    HEADER_BYTES bytes of the output with it: after finish() the header
 *    carries the stream's total sample count and minimum/maximum frame size
 *
 * Example:
 * @code
 * FLACEncoder encoder(PcmFormat{44100, 2, 16});
 * std::vector<uint8_t> out(encoder.get_max_output_bytes());
 * size_t written = 0;
 *
 * if (encoder.write_header(out.data(), out.size(), written) != FLAC_ENCODER_SUCCESS) {
 *     // handle bad config
 * }
 * sink(out.data(), written);
 *
 * while (have_input) {
 *     size_t consumed = 0;
 *     FLACEncoderResult result =
 *         encoder.encode(input, input_len, out.data(), out.size(), consumed, written);
 *     if (result == FLAC_ENCODER_NEED_MORE_DATA) {
 *         // move the leftover bytes to the front of the buffer and read more
 *     } else if (result < 0) {
 *         break;  // handle error
 *     }
 *     sink(out.data(), written);
 *     input += consumed;
 *     input_len -= consumed;
 * }
 *
 * if (encoder.finish(input, input_len, out.data(), out.size(), written) == FLAC_ENCODER_SUCCESS) {
 *     sink(out.data(), written);
 * }
 * @endcode
 *
 * @note Without step 5 the stream is still valid: STREAMINFO's total sample
 *       count and frame sizes stay "unknown" (RFC 9639 SS8.2). The MD5
 *       signature is always left unset. A caller that wants one hashes its
 *       input and writes the digest into the header's last 16 bytes, as
 *       host_examples/wav_to_flac does. At depths other than 8, 16 and 24,
 *       each sample must first be right-justified and sign-extended in its
 *       bytes before hashing.
 */
class FLACEncoder {
public:
    // ========================================
    // Constants
    // ========================================

    /// @brief Bytes write_header() produces: "fLaC" + the STREAMINFO block
    static constexpr size_t HEADER_BYTES = 42;

    /// @brief Smallest FLACEncoderOptions::block_size accepted (RFC 9639 SS8.2)
    static constexpr uint32_t MIN_BLOCK_SIZE = 16;

    /// @brief Largest FLACEncoderOptions::block_size accepted (16-bit field)
    static constexpr uint32_t MAX_BLOCK_SIZE = 65535;

    /// @brief Largest FLACEncoderOptions::max_rice_partition_order accepted
    static constexpr uint8_t MAX_RICE_PARTITION_ORDER = 6;

    /// @brief Whether this build includes Rice partitioning
    ///
    /// Without it (MICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS),
    /// FLACEncoderOptions::max_rice_partition_order is validated, then ignored.
#ifdef MICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS
    static constexpr bool RICE_PARTITIONS_AVAILABLE = false;
#else
    static constexpr bool RICE_PARTITIONS_AVAILABLE = true;
#endif

    // ========================================
    // Lifecycle
    // ========================================

    /// @brief Construct an encoder for one input format
    ///
    /// Always succeeds. The configuration is reported by the first
    /// write_header(), encode() or finish() call. Supported: 1-8 channels, 4-24
    /// bits per sample, and sample rates of 1-1048575 Hz. Rates no frame header
    /// code can carry (above 65535 Hz, other than multiples of 10 up to 655350
    /// Hz and whole kHz up to 255 kHz) are read from STREAMINFO, outside the
    /// streamable subset.
    ///
    /// @param format Layout of the PCM bytes encode() and finish() will receive
    /// @param options Compression settings (defaults work for any format)
    explicit FLACEncoder(const PcmFormat& format,
                         const FLACEncoderOptions& options = FLACEncoderOptions());

    // Non-copyable and non-movable: an encoder holds one stream's state (frame
    // numbering and the STREAMINFO statistics), which a copy would silently
    // fork. Construct it once in place and reset() between streams, passing a
    // new format and options to reset() to reconfigure it.
    FLACEncoder(const FLACEncoder&) = delete;
    FLACEncoder& operator=(const FLACEncoder&) = delete;
    FLACEncoder(FLACEncoder&&) = delete;
    FLACEncoder& operator=(FLACEncoder&&) = delete;

    /// @brief Reset the encoder to its initial state, ready to encode a new stream
    ///
    /// Resets the stream state: the frame number, the total sample count, the
    /// minimum/maximum frame sizes, and the finished flag. The next
    /// write_header() therefore writes a fresh, unfinished header.
    ///
    /// User configuration is preserved across the reset:
    /// - The input format, from the constructor or reset(format, options)
    /// - The compression settings, from the constructor or reset(format, options)
    /// - get_max_output_bytes() and get_input_block_bytes(), which derive from both
    /// - An unsupported configuration's FLAC_ENCODER_ERROR_BAD_CONFIG
    ///
    /// The encoder never allocates, so reset() cannot fail.
    void reset();

    /// @brief Reset the encoder and reconfigure it for a new stream
    ///
    /// Leaves the encoder exactly as FLACEncoder(format, options) would, ending
    /// any stream in progress. Like the constructor, it always succeeds, and an
    /// unsupported configuration is reported by the next write_header(),
    /// encode() or finish() call as FLAC_ENCODER_ERROR_BAD_CONFIG.
    ///
    /// @note get_max_output_bytes() and get_input_block_bytes() follow the new
    ///       configuration: re-check the output buffer against
    ///       get_max_output_bytes() after calling this, since more channels, a
    ///       deeper format or a larger block raises it.
    ///
    /// @param format Layout of the PCM bytes encode() and finish() will receive
    /// @param options Compression settings (defaults work for any format)
    void reset(const PcmFormat& format, const FLACEncoderOptions& options = FLACEncoderOptions());

    // ========================================
    // Core Encoding API
    // ========================================

    /// @brief Write the "fLaC" marker and STREAMINFO block
    ///
    /// May be called at any time. Before finish(), the total sample count and
    /// minimum/maximum frame size are "unknown"; after it, they carry the
    /// stream's values where STREAMINFO's fields can hold them. Every header is
    /// HEADER_BYTES long, so the finished one can overwrite the first in place.
    ///
    /// @param output Output buffer (must not be null)
    /// @param output_size_bytes Size of output; at least HEADER_BYTES
    /// @param[out] bytes_written HEADER_BYTES on success, 0 on error
    /// @return FLAC_ENCODER_SUCCESS or a negative error code
    MICRO_FLAC_NODISCARD
    FLACEncoderResult write_header(uint8_t* output, size_t output_size_bytes,
                                   size_t& bytes_written);

    /// @brief Encode one block of PCM into one FLAC frame
    ///
    /// Consumes exactly get_input_block_bytes() and writes one frame, or, given
    /// less, consumes nothing and returns FLAC_ENCODER_NEED_MORE_DATA. The
    /// output size is checked only once there is a full block.
    ///
    /// @param input Packed PCM (see PcmFormat; may be null only when input_len
    ///        is 0). When bytes_per_sample() is 2 and input_len is not 0, it
    ///        must be 2-byte aligned
    /// @param input_len Bytes available at input
    /// @param output Output buffer (must not be null)
    /// @param output_size_bytes Size of output; at least get_max_output_bytes()
    /// @param[out] bytes_consumed Input bytes consumed: get_input_block_bytes() or 0
    /// @param[out] bytes_written Output bytes written: the frame's size, or 0
    /// @return FLAC_ENCODER_SUCCESS, FLAC_ENCODER_NEED_MORE_DATA, or a negative
    ///         error code
    MICRO_FLAC_NODISCARD
    FLACEncoderResult encode(const uint8_t* input, size_t input_len, uint8_t* output,
                             size_t output_size_bytes, size_t& bytes_consumed,
                             size_t& bytes_written);

    /// @brief Encode the input's remainder as the final frame and end the stream
    ///
    /// input_len is a whole number of sample frames, from 0 (no frame is
    /// written) up to one block. Afterwards encode() and finish() return
    /// FLAC_ENCODER_ERROR_STREAM_FINISHED until reset(), and write_header()
    /// produces the finished header.
    ///
    /// @param input Remaining packed PCM (may be null only when input_len is
    ///        0). When bytes_per_sample() is 2 and input_len is not 0, it must
    ///        be 2-byte aligned
    /// @param input_len Remaining bytes, at most get_input_block_bytes()
    /// @param output Output buffer (must not be null)
    /// @param output_size_bytes Size of output; at least get_max_output_bytes()
    /// @param[out] bytes_written Output bytes written (0 when input_len is 0)
    /// @return FLAC_ENCODER_SUCCESS or a negative error code
    MICRO_FLAC_NODISCARD
    FLACEncoderResult finish(const uint8_t* input, size_t input_len, uint8_t* output,
                             size_t output_size_bytes, size_t& bytes_written);

    // ========================================
    // Configuration
    // ========================================

    /// @brief The current options, from the constructor or reset(format, options)
    /// @return Reference to the stored FLACEncoderOptions
    const FLACEncoderOptions& get_options() const {
        return this->options_;
    }

    // ========================================
    // PCM Format
    // ========================================

    /// @brief The current input format, from the constructor or reset(format, options)
    /// @return Reference to the stored PcmFormat
    const PcmFormat& get_pcm_format() const {
        return this->format_;
    }

    // ========================================
    // Buffer Helpers
    // ========================================

    /// @brief Output buffer size that every call is guaranteed to fit in
    ///
    /// A hard bound on one frame, and at least HEADER_BYTES. Constant until
    /// reset(format, options) reconfigures the encoder.
    ///
    /// @return Bytes, or 0 if the configuration is unsupported
    size_t get_max_output_bytes() const {
        return this->max_output_bytes_;
    }

    /// @brief Input bytes that one encode() call consumes
    /// @return block_size * PcmFormat::bytes_per_frame(), or 0 if the
    ///         configuration is unsupported
    size_t get_input_block_bytes() const {
        return this->input_block_bytes_;
    }

    // ========================================
    // Stream Information
    // ========================================

    /// @brief Samples per channel encoded in the current stream
    /// @return Total samples per channel encoded since construction or reset()
    // Public API accessor; no in-tree caller needs it, but library consumers do.
    // cppcheck-suppress unusedFunction
    uint64_t get_total_samples_encoded() const {
        return this->total_samples_;
    }

private:
    // ========================================
    // Private Constants
    // ========================================

    /// @brief Samples per channel unpacked at a time
    static constexpr uint32_t CHUNK_SAMPLES = 256;

    // ========================================
    // Private Helpers
    // ========================================

    /// @brief Store and validate a configuration, and precompute what encoding needs
    ///
    /// Clears everything a previous configuration derived first, so a rejected
    /// configuration leaves the same state as a freshly constructed encoder.
    void configure(const PcmFormat& format, const FLACEncoderOptions& options);

    /// @brief Validate the configuration, stream state and arguments encode() and finish() share
    MICRO_FLAC_NODISCARD
    FLACEncoderResult check_call(const uint8_t* input, size_t input_len, const uint8_t* output);

    /// @brief Check the frame number and output capacity for one more frame
    MICRO_FLAC_NODISCARD
    FLACEncoderResult check_frame_room(size_t output_size_bytes) const;

    /// @brief Encode `num_samples` samples per channel from `input` as one frame
    FLACEncoderResult encode_frame(const uint8_t* input, uint32_t num_samples, uint8_t* output,
                                   size_t output_size_bytes, size_t& bytes_written);

    /// @brief Write this frame's header (RFC 9639 SS9.1) at `output`
    ///
    /// @return The header's length; the frame's subframes follow it
    uint8_t start_frame(uint8_t* output, uint32_t num_samples, uint8_t channel_assignment) const;

    /// @brief Advance the stream state past a finished frame
    ///
    /// @param frame_bytes The frame's size, or 0 if it overflowed the output
    MICRO_FLAC_NODISCARD
    FLACEncoderResult finish_frame(size_t frame_bytes, uint32_t num_samples, size_t& bytes_written);

    // ========================================
    // Member Variables
    // ========================================

    // Ordered for performance: chunk_ ends the 32-bit fields so the fields before it stay
    // within one Xtensa load's offset reach, which the per-frame bookkeeping relies on
    // Struct fields
    PcmFormat format_;            // As last passed to configure()
    FLACEncoderOptions options_;  // As last passed to configure()

    // 64-bit fields
    uint64_t total_samples_{0};  // Samples per channel encoded in this stream

    // size_t fields
    size_t max_output_bytes_{0};  // See get_max_output_bytes(); 0 for an unsupported configuration
    size_t input_block_bytes_{
        0};  // See get_input_block_bytes(); 0 for an unsupported configuration

    // 32-bit fields
    uint32_t frame_number_{0};            // Number of the next frame
    uint32_t min_frame_bytes_{0};         // Smallest frame written so far (0 = none yet)
    uint32_t max_frame_bytes_{0};         // Largest frame written so far (0 = none yet)
    int32_t chunk_[2 * CHUNK_SAMPLES]{};  // Unpacked samples: a stereo pair, or one channel in
                                          // the first half

    // 8-bit fields
    FLACEncoderResult config_result_{FLAC_ENCODER_ERROR_BAD_CONFIG};  // configure()'s verdict
    uint8_t bits_per_sample_code_{0};                                 // Frame header bit-depth code
    uint8_t sample_rate_code_{0};           // Frame header sample-rate code
    uint8_t sample_rate_extra_[2]{};        // Its extra bytes, big-endian
    uint8_t sample_rate_extra_len_{0};      // 0, 1, or 2
    uint8_t max_partition_order_{0};        // Largest Rice partition order tried; 0 when off
    bool finished_{false};                  // Whether finish() has ended this stream
    bool stereo_estimation_enabled_{true};  // false forces independent stereo; set only by
                                            // test_encoder_config (-fno-access-control)
};

}  // namespace micro_flac
