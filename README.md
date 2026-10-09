# microFLAC - Embedded FLAC Decoder and Encoder

[![CI](https://github.com/esphome-libs/micro-flac/actions/workflows/ci.yml/badge.svg)](https://github.com/esphome-libs/micro-flac/actions/workflows/ci.yml)
[![Component Registry](https://components.espressif.com/components/esphome/micro-flac/badge.svg)](https://components.espressif.com/components/esphome/micro-flac)

A FLAC (Free Lossless Audio Codec) decoder and encoder optimized for ESP32 embedded devices. The decoder supports both native FLAC and Ogg FLAC containers with automatic format detection; the encoder produces native FLAC from 4-24 bit PCM in 1-8 channels. Designed as an ESP-IDF component with PSRAM support and Xtensa assembly optimizations for ESP32/ESP32-S3.

[![A project from the Open Home Foundation](https://www.openhomefoundation.org/badges/ohf-project.png)](https://www.openhomefoundation.org/)

## Features

### Decoding

- **Native FLAC and Ogg FLAC**: Automatic container detection from first 4 bytes
- **Unified streaming API**: Single `decode()` method handles container detection, header parsing, and frame decoding
- **All FLAC bit depths**: 8-bit through 32-bit samples
- **CRC validation**: Optional frame integrity checking with CRC-8 (header) and CRC-16 (data)
- **PSRAM support**: Configurable memory placement with automatic fallback
- **Metadata extraction**: Album art, Vorbis comments, seektable, and more with configurable size limits
- **Xtensa optimizations**: LPC prediction with hardware multiply-accumulate on ESP32/ESP32-S3; C fallback on RISC-V chips (C3, C6, P4) and host

### Encoding

- **4-24 bit PCM input, 1-8 channels**: Packed interleaved bytes in exactly the layout the decoder outputs (`PcmFormat`)
- **Fixed predictors, plus opt-in LPC**: Fixed predictors (orders 0-4) always; linear prediction up to order 12 on request, typically 7-9% smaller on music. All integer arithmetic, so output is identical on every platform
- **Opt-in Rice partitioning and LPC stereo search**: With both and LPC order 8, output is within 0.01% of reference `flac -5`
- **Stereo decorrelation**: Each frame takes the cheapest of the four channel assignments
- **Wasted-bits detection**: 16-bit audio in a 24-bit stream codes nearly as small as a 16-bit stream
- **No buffering, no allocation**: `encode()` reads one block straight from the caller's buffer; all working memory is the ~2.1 KB object plus up to ~4.3 KB of task stack
- **Finished stream header**: After `finish()`, a same-size header carrying the total sample count and frame-size range can overwrite the first

## Quick Start

### ESP-IDF Component

1. Add as a component to your project:

```bash
cd components
git clone https://github.com/esphome-libs/micro-flac.git
```

1. Configure via menuconfig:

```bash
pio run -e esp32s3 --target menuconfig
# Navigate to: Component config → microFLAC
```

1. Include and use:

```cpp
#include "micro_flac/flac_decoder.h"

using namespace micro_flac;

FLACDecoder decoder;

// Optional: configure metadata limits before first decode() call
decoder.set_max_metadata_size(FLAC_METADATA_TYPE_PICTURE, 50 * 1024);  // 50KB album art
decoder.set_max_metadata_size(FLAC_METADATA_TYPE_VORBIS_COMMENT, 4096);

// Decode in a loop (works with both .flac and .oga files automatically)
uint8_t* output = nullptr;
size_t output_size_bytes = 0;

while (have_data) {
    size_t bytes_consumed = 0, samples_decoded = 0;
    auto result = decoder.decode(input, input_len, output, output_size_bytes,
                                 bytes_consumed, samples_decoded);
    input += bytes_consumed;
    input_len -= bytes_consumed;

    if (result == FLAC_DECODER_HEADER_READY) {
        // Stream info now available, allocate output buffer
        const auto& info = decoder.get_stream_info();
        output_size_bytes = info.max_block_size() * info.num_channels()
                          * info.bytes_per_sample();  // or use get_output_buffer_size_samples() for int32_t* path
        output = new uint8_t[output_size_bytes];
    } else if (result == FLAC_DECODER_SUCCESS) {
        // Process samples_decoded interleaved PCM samples in output
    } else if (result == FLAC_DECODER_NEED_MORE_DATA) {
        // Read more data into buffer and try again
    } else if (result == FLAC_DECODER_END_OF_STREAM) {
        break;
    } else {
        break;  // Negative values are errors
    }
}
delete[] output;
```

### Encoding Example

```cpp
#include "micro_flac/flac_encoder.h"

using namespace micro_flac;

// 44.1kHz 16-bit stereo input; FLACEncoderOptions (block size, LPC, Rice
// partitioning, wasted bits) can be passed as a second argument.
FLACEncoder encoder(PcmFormat{44100, 2, 16});

std::vector<uint8_t> out(encoder.get_max_output_bytes());  // fits the header and any frame
size_t bytes_written = 0;

// The first call reports an unsupported configuration
if (encoder.write_header(out.data(), out.size(), bytes_written) != FLAC_ENCODER_SUCCESS) {
    // handle error
}
// write out.data()[0 .. bytes_written) to the output stream/file

while (have_input) {
    size_t bytes_consumed = 0;
    auto result = encoder.encode(input, input_len, out.data(), out.size(),
                                 bytes_consumed, bytes_written);
    if (result == FLAC_ENCODER_NEED_MORE_DATA) {
        // Less than one block (encoder.get_input_block_bytes()) is available and nothing
        // was consumed: append more input and call again
    } else if (result == FLAC_ENCODER_SUCCESS) {
        // write out.data()[0 .. bytes_written) to the output stream/file
        input += bytes_consumed;
        input_len -= bytes_consumed;
    } else {
        break;  // negative values are errors
    }
}

// Encode whatever is left (less than one block) as the final frame
if (encoder.finish(input, input_len, out.data(), out.size(), bytes_written) == FLAC_ENCODER_SUCCESS) {
    // write out.data()[0 .. bytes_written) to the output stream/file
}

// Optional, for seekable output: the finished header carries the total sample
// count and frame sizes, and is the same size as the first one
if (encoder.write_header(out.data(), out.size(), bytes_written) == FLAC_ENCODER_SUCCESS) {
    // overwrite the first bytes_written (FLACEncoder::HEADER_BYTES) of the output with it
}
```

The stream is decodable exactly as emitted. The header written before `finish()` leaves STREAMINFO's total sample count and minimum/maximum frame size as "unknown" (spec-legal); rewriting it after `finish()` fills them in. The MD5 signature is always left unset; `host_examples/wav_to_flac` shows how a caller computes it and writes it into the finished header's last 16 bytes.

### PlatformIO

Add to `platformio.ini`:

```ini
[env:esp32dev]
platform = espressif32
framework = espidf
lib_deps =
    https://github.com/esphome-libs/micro-flac.git

# Configure memory preference
build_flags =
    -DMICRO_FLAC_MEMORY_PREFER_PSRAM
```

### Host Build (Linux/macOS)

```bash
cd host_examples/flac_to_wav
cmake -B build && cmake --build build
./build/flac_to_wav input.flac output.wav       # Native FLAC
./build/flac_to_wav input.oga output.wav        # Ogg FLAC

cd ../wav_to_flac
cmake -B build && cmake --build build
./build/wav_to_flac input.wav output.flac       # Encode
```

## API Reference

### Decoder Methods

| Method | Description |
| ------ | ----------- |
| `decode(input, len, uint8_t* output, output_size_bytes, bytes_consumed, samples_decoded)` | Decode with native byte packing (e.g., 2 bytes for 16-bit, 3 for 24-bit). `output_size_bytes` in bytes (`max_block_size * channels * bytes_per_sample`). |
| `decode(input, len, int32_t* output, output_size_samples, bytes_consumed, samples_decoded)` | Decode with 32-bit left-justified output (all bit depths → 4 bytes). `output_size_samples` in samples (`max_block_size * channels` or `get_output_buffer_size_samples()`). |
| `get_stream_info()` | Get stream info struct (sample rate, channels, bit depth, etc.) after HEADER_READY. Use `get_stream_info().is_valid()` to check if parsed. |
| `get_output_buffer_size_samples()` | Get required output buffer size in samples (max_block_size * num_channels) |
| `reset()` | Reset decoder state for decoding a new stream (preserves configuration) |
| `set_crc_check_enabled(bool)` | Enable/disable CRC validation |
| `set_max_metadata_size(type, size)` | Set max stored size for a metadata type (call before first `decode()`) |
| `get_metadata_block(type)` | Get a specific metadata block by type (album art, tags, etc.) |
| `get_metadata_blocks()` | Get all stored metadata blocks |

> **Note:** The decoder always outputs **signed** samples at all bit depths, including 8-bit. WAV files require unsigned 8-bit samples - consumers writing WAV must add 128 to each byte themselves.

### FLACStreamInfo Methods

Available after `decode()` returns `FLAC_DECODER_HEADER_READY` via `decoder.get_stream_info()`:

| Method | Description |
| ------ | ----------- |
| `sample_rate()` | Sample rate in Hz (e.g., 44100, 48000) |
| `num_channels()` | Channel count (1=mono, 2=stereo, etc.) |
| `bits_per_sample()` | Bit depth (8, 16, 24, or 32) |
| `min_block_size()` / `max_block_size()` | Block size range in samples |
| `total_samples_per_channel()` | Total samples per channel (0 if unknown) |
| `bytes_per_sample()` | Bytes per sample, rounded up (e.g., 2 for 16-bit, 3 for 24-bit) |
| `md5_signature()` | Pointer to 16-byte MD5 signature of unencoded audio data |
| `is_valid()` | Whether STREAMINFO has been parsed |

### Decoder Result Codes

`decode()` returns `FLACDecoderResult`: non-negative values indicate success/informational states, negative values indicate errors. See `flac_decoder.h` for the full enum.

| Code | Value | Description |
| ---- | ----- | ----------- |
| `FLAC_DECODER_SUCCESS` | 0 | Frame decoded successfully |
| `FLAC_DECODER_HEADER_READY` | 1 | Header parsed, stream info available (allocate output buffer now) |
| `FLAC_DECODER_END_OF_STREAM` | 2 | No more frames to decode |
| `FLAC_DECODER_NEED_MORE_DATA` | 3 | Not enough input data; feed more and call `decode()` again |

### Encoder Methods

`FLACEncoder` encodes interleaved 4-24 bit PCM in 1-8 channels to native FLAC with a fixed block size: every frame but the last holds exactly `block_size` samples per channel. It is configured at construction, or later with `reset(format, options)`, from two structs:

- **`PcmFormat`** (`micro_flac/pcm_format.h`) describes the input. Construct it as `PcmFormat{sample_rate, num_channels, bits_per_sample}` (1-1048575 Hz, 1-8 channels, 4-24 bits) and read it back through `sample_rate()`, `num_channels()`, `bits_per_sample()`, `bytes_per_sample()`, `bytes_per_frame()` and `is_valid()`, as in the other micro-* libraries. Samples are interleaved, little-endian, signed and `ceil(bits_per_sample() / 8)` bytes wide, with other depths left-justified (a 12-bit sample fills the top 12 bits of its 2 bytes). This is exactly what the decoder's `uint8_t*` `decode()` produces. 2-byte samples must be 2-byte aligned.
- **`FLACEncoderOptions`** controls compression (see [Encoding Performance](#encoding-performance) for the costs):
  - `block_size` (16-65535, default 4096).
  - `lpc` (default off): also try linear prediction on each subframe, keeping it where it is estimated smaller. Typically 7-9% smaller on 16-bit music. Streams of 17-24 bits use 64-bit arithmetic, much slower on an ESP32 (see [Encoding Performance](#encoding-performance)).
  - `max_lpc_order` (1-12, default 8): the highest LPC order tried. Orders 4, 8 and 12 come out about 6.8%, 8.6% and 9% smaller than the fixed predictors alone.
  - `lpc_stereo_search` (default off): with `lpc` on stereo, choose each frame's channel assignment after designing LPC for all four candidates (left, right, mid, side) rather than from the fixed predictors. 0.05-0.25% smaller.
  - `max_rice_partition_order` (0-6, default 0): split each subframe's residuals into up to 2^order partitions with their own Rice parameters. About 1% smaller at block size 4096, 0.3% at 1152.
  - `wasted_bits` (default on): code a subframe whose samples all end in the same k zero bits k bits shallower (RFC 9639 §9.2.2), as with 16-bit audio in a 24-bit stream.

Every format is unpacked 256 samples per channel at a time into a 2 KB buffer inside the encoder object, which never allocates.

| Method | Description |
| ------ | ----------- |
| `FLACEncoder(format, options = {})` | Construct for one input format. Never fails and allocates nothing; the configuration is validated here and reported by the first call below |
| `write_header(output, output_size_bytes, bytes_written)` | Write the `HEADER_BYTES` (42) byte `fLaC` marker and STREAMINFO block. Before `finish()` the total sample count and frame sizes are "unknown"; afterwards they are filled in (unless too large for STREAMINFO's fields), at the same size, so the finished header can overwrite the first one |
| `encode(input, input_len, output, output_size_bytes, bytes_consumed, bytes_written)` | Consume exactly one block (`get_input_block_bytes()`) and write one frame, or return `FLAC_ENCODER_NEED_MORE_DATA` without consuming anything if `input_len` is shorter than a block |
| `finish(input, input_len, output, output_size_bytes, bytes_written)` | Encode the remainder (0 bytes up to one block, whole sample frames) as the final frame and end the stream |
| `reset()` | Reset stream state for encoding a new stream (preserves configuration) |
| `reset(format, options = {})` | Reset and reconfigure, leaving the encoder as the constructor would. Never fails; a rejected configuration is reported by the next call. Re-check the output buffer against `get_max_output_bytes()` afterwards |
| `get_max_output_bytes()` | Output buffer size that fits the header and every frame; constant until `reset(format, options)` reconfigures the encoder |
| `get_input_block_bytes()` | Input bytes one `encode()` call consumes (`block_size * num_channels * bytes_per_sample`) |
| `get_total_samples_encoded()` | Samples per channel encoded in the current stream |
| `get_pcm_format()` / `get_options()` | The current configuration, from the constructor or the last `reset(format, options)` |

### Encoder Result Codes

Every call returns `FLACEncoderResult`: non-negative values are success/informational, negative values are errors. An error consumes no input and reports no output (`bytes_written` is 0; the output buffer's contents are unspecified). `OUTPUT_BUFFER_TOO_SMALL` and `INVALID_ARGUMENT` can be fixed and retried; `BAD_CONFIG` persists until `reset(format, options)` supplies a supported configuration; after `STREAM_FINISHED` or `STREAM_TOO_LONG`, `reset()` starts a new stream (`finish()` with no input still ends a too-long stream cleanly).

| Code | Value | Description |
| ---- | ----- | ----------- |
| `FLAC_ENCODER_SUCCESS` | 0 | The call did its work (check `bytes_written`) |
| `FLAC_ENCODER_NEED_MORE_DATA` | 1 | `encode()` was given less than one block; nothing consumed |
| `FLAC_ENCODER_ERROR_OUTPUT_BUFFER_TOO_SMALL` | -1 | Output buffer smaller than `get_max_output_bytes()` (or `HEADER_BYTES` for `write_header()`) |
| `FLAC_ENCODER_ERROR_INVALID_ARGUMENT` | -2 | Null pointer, misaligned 2-byte-sample input, or a `finish()` remainder that is not whole sample frames or is longer than one block |
| `FLAC_ENCODER_ERROR_BAD_CONFIG` | -3 | Unsupported `PcmFormat` or `FLACEncoderOptions`. Persists until `reset(format, options)` with a supported configuration |
| `FLAC_ENCODER_ERROR_STREAM_FINISHED` | -4 | `encode()` or `finish()` after `finish()`; call `reset()` to start a new stream |
| `FLAC_ENCODER_ERROR_STREAM_TOO_LONG` | -5 | The next frame number would exceed the format's 2^31 - 1 |

### Output Bound Guarantee

`get_max_output_bytes()` is a hard upper bound on what any call can write. A call with a frame to write checks the output size first and returns `FLAC_ENCODER_ERROR_OUTPUT_BUFFER_TOO_SMALL` without touching `output` if it is smaller. The bound holds for every input because a FIXED or LPC subframe is written only when it is provably smaller than VERBATIM.

## Configuration

Configure via ESP-IDF menuconfig (`Component config → microFLAC`) or compile flags. Host CMake builds take the same feature switches as options (`-DMICRO_FLAC_ENABLE_OGG=OFF`, `-DMICRO_FLAC_ENCODER_ENABLE_LPC=OFF`, `-DMICRO_FLAC_ENCODER_ENABLE_RICE_PARTITIONS=OFF`).

| Option | Default | Kconfig / Compile Flag | Notes |
| ------ | ------- | ---------------------- | ----- |
| Memory preference | Prefer PSRAM | `CONFIG_MICRO_FLAC_PREFER_PSRAM` / `-DMICRO_FLAC_MEMORY_PREFER_PSRAM` | Also: prefer internal, PSRAM-only, internal-only |
| CRC checking | Enabled | Runtime: `set_crc_check_enabled(bool)` | CRC-8 (header) and CRC-16 (data) |
| Xtensa assembly | Enabled (ESP32/S3) | `CONFIG_MICRO_FLAC_ENABLE_XTENSA_ASM` | MULL/MULSH and hardware loops for LPC |
| Ogg FLAC support | Enabled | `CONFIG_MICRO_FLAC_ENABLE_OGG` / `-DMICRO_FLAC_DISABLE_OGG` | Disabling saves ~3-5 KB flash |
| Encoder LPC | Enabled | `CONFIG_MICRO_FLAC_ENCODER_ENABLE_LPC` / `-DMICRO_FLAC_ENCODER_DISABLE_LPC` | Disabling saves ~19 KB flash (ESP32-S3); the LPC options are then ignored. `FLACEncoder::LPC_AVAILABLE` reports it |
| Encoder Rice partitioning | Enabled | `CONFIG_MICRO_FLAC_ENCODER_ENABLE_RICE_PARTITIONS` / `-DMICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS` | Disabling saves ~7 KB flash (ESP32-S3); `max_rice_partition_order` is then ignored. `FLACEncoder::RICE_PARTITIONS_AVAILABLE` reports it |

## Performance

Decoding performance for 48kHz stereo audio (full frame, CRC enabled):

| Chip | Clock | 16-bit | 24-bit |
| ---- | ----- | ------ | ------ |
| ESP32 (internal SRAM) | 240 MHz | ~12x realtime | n/a |
| ESP32 (PSRAM) | 240 MHz | ~8x realtime | n/a |
| ESP32-S3 | 240 MHz | ~30x realtime | ~19x realtime |
| ESP32-P4 | 360 MHz | ~25x realtime | ~18x realtime |

ESP32-S3 and ESP32-P4 numbers are measured with the working buffer in PSRAM (the default); PSRAM is fast enough on these chips that switching to internal SRAM only saves ~2-4% on the S3 and well under 1% on the P4. On the original ESP32, PSRAM is much slower than internal SRAM, so placing the working buffer in internal memory (`CONFIG_MICRO_FLAC_PREFER_INTERNAL=y`) is roughly 30-35% faster and is recommended for performance-sensitive use. Performance also varies with block size, prediction order, and sample depth (24-bit requires 64-bit arithmetic). See [examples/decode_benchmark/README.md](examples/decode_benchmark/README.md) for detailed benchmarks, streaming overhead analysis, and instructions for running your own.

### Encoding Performance

Encoding the 30-second 48 kHz stereo clip on an ESP32-S3 at 240 MHz (`examples/encode_benchmark`). Sizes are relative to the default settings at block size 4096.

| Settings | 16-bit | 24-bit | Size |
| -------- | ------ | ------ | ---- |
| Defaults, block size 4096 | 21.3x realtime | 15.9x realtime | baseline |
| Defaults, block size 1152 | 20.9x realtime | 15.7x realtime | same |
| `max_rice_partition_order` 6 | 17.9x realtime | 13.5x realtime | -0.6% |
| `lpc`, order 4 | 11.3x realtime | n/a | -4.6% |
| `lpc`, order 8 | 9.1x realtime | 4.1x realtime | -4.8% |
| `lpc`, order 12 | 8.1x realtime | n/a | -4.8% |
| `lpc` order 8 + `lpc_stereo_search` | 6.5x realtime | n/a | -5.2% |
| `lpc` order 8 + search + partitions | 6.0x realtime | n/a | -5.8% |

On a typical music corpus LPC gains more (7-9%) than on this clip. A host encodes several times faster. LPC on 24-bit audio takes 64-bit arithmetic, which Xtensa builds from 32-bit halves, hence its lower speed.

### Memory Usage

| Allocation | Size | Notes |
| ---------- | ---- | ----- |
| Decoder object | ~200 bytes | Stack or heap |
| Block samples buffer | `max_block_size × channels × 4` | Typically 16-64KB |
| Metadata blocks | Variable | Configurable per type |
| Output buffer | `max_block_size × channels × bytes_per_sample` | Allocated by user |
| Encoder object | ~2.1 KB | Stack or heap. `encode()` itself needs up to about 1.6 KB of task stack, 2.3 KB with partitioning, 3.4 KB with LPC, or 4.3 KB with the stereo search |
| Encoder output buffer | `get_max_output_bytes()` | Allocated by user; fits one worst-case frame |
| Encoder input | One block (`get_input_block_bytes()`) | The caller's buffer, unpacked a chunk at a time |

**PSRAM is recommended** for the decoder's block samples buffer to conserve internal RAM.

## Testing

The decoder is validated against the [FLAC test suite](https://github.com/ietf-wg-cellar/flac-test-files) with bit-perfect output compared to ffmpeg.

```bash
cd host_examples/flac_to_wav
cmake -B build && cmake --build build
python3 test_flac_decoder.py
```

Tests validate bit-perfect PCM output, MD5 signatures, various bit depths (8-32), sample rates, channel counts (1-8), embedded album art, byte-by-byte streaming, and Ogg FLAC containers. See [host_examples/flac_to_wav/TESTING.md](host_examples/flac_to_wav/TESTING.md) for full test suite details, test categories, and troubleshooting.

```bash
# ESP32 benchmark
cd examples/decode_benchmark
pio run -e esp32s3 -t upload -t monitor
```

The encoder is validated with a round-trip suite: synthetic and real-audio WAV sources are encoded, decoded back with the decoder above (and, when available, `ffmpeg`/`flac`), and checked for bit-exact PCM.

```bash
cd host_examples/wav_to_flac
cmake -B build && cmake --build build
python3 test_flac_encoder.py
```

Unit tests for the bit writer and the encoder's configuration and argument handling live in `tests/encoder`:

```bash
cd tests/encoder
cmake -B build && cmake --build build
ctest --test-dir build
```

```bash
# ESP32 benchmark
cd examples/encode_benchmark
pio run -e esp32s3 -t upload -t monitor
```

## Advanced Features

### 32-bit Sample Output Mode

For embedded systems, unpacking 24-bit samples (3 bytes each) can be inefficient. Use the `int32_t*` overload of `decode()` to get left-justified (MSB-aligned) 32-bit samples:

```cpp
// Allocate a 32-bit output buffer after HEADER_READY
size_t output_size_samples = decoder.get_output_buffer_size_samples();
int32_t* output = new int32_t[output_size_samples];

// Use the int32_t* decode overload - 24-bit audio is shifted left by 8, 16-bit by 16, etc.
auto result = decoder.decode(input, input_len, output, output_size_samples,
                             bytes_consumed, samples_decoded);
```

### Metadata Extraction

Configure size limits before the first `decode()` call:

```cpp
// Enable album art up to 50KB
decoder.set_max_metadata_size(FLAC_METADATA_TYPE_PICTURE, 50 * 1024);

// Increase Vorbis comment storage
decoder.set_max_metadata_size(FLAC_METADATA_TYPE_VORBIS_COMMENT, 4096);

// After decode() returns FLAC_DECODER_HEADER_READY, access metadata:
const FLACMetadataBlock* art = decoder.get_metadata_block(FLAC_METADATA_TYPE_PICTURE);
if (art) {
    // art->data contains the raw FLAC PICTURE block; parse per the FLAC spec
    // to extract the embedded image (type, MIME, description, then image bytes)
    // art->length is the total block size in bytes
}

const FLACMetadataBlock* tags = decoder.get_metadata_block(FLAC_METADATA_TYPE_VORBIS_COMMENT);
if (tags) {
    // Parse Vorbis comments: ARTIST=..., ALBUM=..., etc.
}
```

Default limits (conservative for embedded): STREAMINFO is always stored; all others are 0 bytes (skipped by default).

### Custom Memory Allocation

Override `FLAC_MALLOC` and `FLAC_FREE` at compile time (`-DFLAC_MALLOC=my_custom_malloc -DFLAC_FREE=my_custom_free`). Custom functions must match `void* malloc(size_t)` and `void free(void*)` signatures.

## Known Limitations

- **No seeking support**: Decoder is designed for streaming, not random access
- **No automatic MD5 validation**: The MD5 signature is extracted from STREAMINFO and accessible via `get_stream_info().md5_signature()`, but not automatically validated during decoding
- **Encoder tops out at 24-bit**: Any depth outside 4-24 is `FLAC_ENCODER_ERROR_BAD_CONFIG`; 32-bit input would need a 64-bit residual path
- **Encoder uses a fixed block size**: Every frame but the last is `block_size` samples; there is no variable-blocksize mode
- **Streamable subset is up to the caller**: Block sizes above 16384, or above 4608 at 48 kHz or below, bit depths other than 8/12/16/20/24, and sample rates no frame header code can carry (above 65535 Hz, other than multiples of 10 up to 655350 Hz and whole kHz up to 255 kHz; these are read from STREAMINFO) fall outside RFC 9639's streamable subset (§7), which a strict subset-only decoder may refuse. The defaults are within it
- **Encoder writes no MD5 signature**: STREAMINFO's MD5 is left zeroed ("unknown"). The caller can hash the input and patch the digest into the finished header, as `wav_to_flac` does
- **Encoder output is native FLAC only**: No Ogg FLAC container, seektables, or Vorbis comments
- **Encoder has no Xtensa assembly**: C implementation on all targets

## License

Apache License 2.0

## Links

- [FLAC Format Specification](https://xiph.org/flac/format.html)
- [Nayuki's Simple FLAC Implementation](https://www.nayuki.io/res/simple-flac-implementation/) (original basis)
- [Mike Hansen's C++ FLAC decoder port](https://github.com/synesthesiam/flac-decoder)
- [FLAC Test Suite](https://github.com/ietf-wg-cellar/flac-test-files)
