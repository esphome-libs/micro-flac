# microFLAC - Internal Architecture

Internal documentation for developers working on the decoder and encoder internals. For the public API and usage guide, see the [root README](../README.md).

## Origins

Based on [Nayuki's Simple FLAC Implementation](https://www.nayuki.io/res/simple-flac-implementation/) and [Mike Hansen's C++ FLAC decoder port](https://github.com/synesthesiam/flac-decoder), rewritten and extensively optimized for embedded systems.

## File Organization

Files are named for the FLAC concept they handle. A concept both directions touch keeps both in one file pair (`frame_header` parses and writes). Each class file holds its own pipeline.

### Public API

- `include/micro_flac/flac_decoder.h` - `FLACDecoder`, `FLACStreamInfo`, `FLACMetadataBlock`, result codes
- `include/micro_flac/flac_encoder.h` - `FLACEncoder`, `FLACEncoderOptions`, result codes
- `include/micro_flac/pcm_format.h` - `PcmFormat`, the packed PCM layout the encoder reads

### Shared

- `flac_format.h` - Every RFC 9639 constant: code tables, subframe types, channel assignments, field widths and limits (except the CRC lookup tables in `crc.cpp` and the few values the public headers expose)
- `frame_header.h` / `frame_header.cpp` - Parsing (`compute_frame_header_length()`, `parse_frame_header()`) and writing (`select_sample_rate_code()`, `select_bits_per_sample_code()`, `write_frame_header()`)
- `pcm_packing.h` / `pcm_packing.cpp` - Packing decoded samples to interleaved PCM (`write_samples()`, see [Output](#output)) and unpacking the encoder's input (`unpack_channel()`, `unpack_stereo()`, `unpack_mid()`, `unpack_side()`, `unpack_sample()`)
- `bit_reader.h` / `bit_writer.h` - Header-only MSB-first bit-stream primitives on stack-local state (`BitReaderLocal`, `BitWriterLocal`), including Rice codes. Header-only so `FLAC_ALWAYS_INLINE` is honored at every call site
- `crc.h` / `crc.cpp` - CRC-8 and CRC-16 lookup tables and functions

### Decoder

- `flac_decoder.cpp` - State machine, container detection, header/metadata parsing, subframe and residual decoding
- `decorrelation.h` / `decorrelation.cpp` - `apply_channel_decorrelation()` for the LEFT_SIDE, RIGHT_SIDE, and MID_SIDE joint stereo modes
- `lpc.h` / `lpc.cpp` - Overflow detection and 32-bit/64-bit LPC restoration
- `xtensa/lpc_xtensa.h`, `xtensa/lpc_32_xtensa.S`, `xtensa/lpc_64_xtensa.S` - LPC restoration in Xtensa assembly (ESP32 and ESP32-S3 only)

### Encoder

- `flac_encoder.cpp` - `FLACEncoder`: stream header, frame assembly, the two-pass predictor and stereo-assignment selection, and subframe encoding (see [Encoding](#encoding) below)

### Utilities

- `alloc.h` - Memory allocation macros (`FLAC_MALLOC` / `FLAC_FREE`) with ESP-IDF PSRAM support
- `compiler.h` - Compiler hints (optimization, inlining, branch prediction, aliasing) and bit buffer constants
- `wrapping_arithmetic.h` - uint32_t-based wrapping arithmetic helpers (`wadd32`, `wsub32`, `wshl32`, `wshr32`, `u32`) for FLAC's modulo-2^bps semantics

## Decode State Machine

The decoder uses a multi-level state machine that makes all parsing resumable across `decode()` calls, enabling streaming with arbitrarily small input buffers.

### Top-Level Phases (`DecodePhase`)

```text
DETECT_CONTAINER ──→ HEADER ──→ AUDIO ──→ DONE
```

- **DETECT_CONTAINER**: Accumulates the first 4 bytes to identify `"fLaC"` (native FLAC) or `"OggS"` (Ogg FLAC), then delegates to the appropriate decode path
- **HEADER**: Parses STREAMINFO and other metadata blocks. Returns `HEADER_READY` when complete
- **AUDIO**: Decodes frames until end of stream
- **DONE**: Terminal state, always returns `END_OF_STREAM`

### Container Paths

```text
                    ┌─────────────────────┐
                    │  Container Detect    │
                    │  (first 4 bytes)     │
                    └──────────┬──────────┘
                               │
                ┌──────────────┴──────────────┐
                │                             │
                ▼                             ▼
       ┌────────────────┐           ┌─────────────────┐
       │  decode_native │           │   decode_ogg    │
       │  ("fLaC")      │           │   ("OggS")      │
       └────────┬───────┘           └────────┬────────┘
                │                            │
                │                   ┌────────┴────────┐
                │                   │  Ogg Demuxer    │
                │                   │  (strips pages, │
                │                   │   BOS prefix)   │
                │                   └────────┬────────┘
                │                            │
                └──────────┬─────────────────┘
                           │
                           ▼
                  ┌────────────────┐
                  │ Header/Frame   │
                  │ Parsing        │
                  │ (shared path)  │
                  └────────────────┘
```

The Ogg path uses `micro_ogg::OggDemuxer` to strip Ogg page framing and the 9-byte BOS prefix (`0x7F "FLAC" 0x01 ...`), then delegates the raw FLAC data to `decode_native` for the actual header/frame parsing.

### Frame Decode Stages (`FrameDecodeStage`)

```text
IDLE ──→ FRAME_HEADER ──→ SUBFRAME ──→ FRAME_FOOTER ──→ IDLE
```

Each stage is resumable. The decoder saves its position and returns `NEED_MORE_DATA` if the input is exhausted mid-stage.

- **FRAME_HEADER**: Accumulates header bytes, then delegates to `parse_frame_header()` (in `frame_header.cpp`) for parsing and validation
- **SUBFRAME**: Decodes all channels' subframes (constant, verbatim, fixed, LPC + residuals)
- **FRAME_FOOTER**: Reads CRC-16 and validates frame integrity

### Subframe Decode Stages (`SubframeDecodeStage`)

```text
SUBFRAME_HEADER ──→ SUBFRAME_CONSTANT
                  │  SUBFRAME_VERBATIM
                  │  SUBFRAME_WARMUP ──→ LPC_PARAMS ──→ RESIDUAL_HEADER
                  │                  └──→ RESIDUAL_HEADER
                  │
                  └──→ RESIDUAL_HEADER ──→ RESIDUAL_PARTITION_PARAM ──→ RESIDUAL_SAMPLES
                                                                     └→ RESIDUAL_ESCAPE_BITS ──→ RESIDUAL_SAMPLES
```

Subframe types:

- **Constant**: Single value repeated for entire block
- **Verbatim**: Raw uncompressed samples
- **Fixed**: Fixed-order linear prediction (orders 0-4)
- **LPC**: Arbitrary-order linear prediction with encoded coefficients

After all subframes are decoded, channel decorrelation is applied via `apply_channel_decorrelation()` (in `decorrelation.cpp`) for mid-side, left-side, or right-side stereo, followed by CRC-16 validation of the frame.

## Encoding

`FLACEncoder` has no state machine and never buffers input or allocates. `encode()` consumes exactly one block from the caller's buffer and writes one frame, or consumes nothing (`FLAC_ENCODER_NEED_MORE_DATA`); `finish()` writes the remainder as a shorter final frame. Between calls it carries only the stream's bookkeeping: the frame number (the stream is fixed-blocksize), the sample count, and the frame-size range the finished STREAMINFO reports. The constructor validates and precomputes, and every public call reports its verdict.

### Chunked Unpacking

Every format takes the same path. The input is unpacked `CHUNK_SAMPLES` (256) samples per channel at a time into `chunk_`, a 2 KB buffer inside the object, and every pass works on those chunks. A walk over one signal reads it through `PackedSignal`: `unpack_channel()` reads one channel, and `unpack_mid()`/`unpack_side()` derive mid or side straight from the packed stereo input in one pass (`pcm_packing.cpp`). Stereo Pass A instead unpacks left and right once per chunk (`unpack_stereo()`) and derives mid and side in place. One chunk layout means one instantiation of the per-order code for every format, with no block-sized buffer. The cost is unpacking the input again for each walk over a subframe: on the ESP32-S3, 16-bit stereo encodes 5-7% slower with fixed predictors than a dedicated path that read `int16_t` in place, which cost 38 KB of flash. Chunks of 512 samples would be under 1% faster than 256, for 2 KB more per object. The scan, the cascade and the Rice writer are tuned against Xtensa's register budget; check the disassembly of `scan_range()`, `write_subframe()` and the unpack loops after changing them.

### Two-Pass Algorithm

Fixed-predictor residuals are computed as a running difference cascade: the order-o residual is the o-th difference of the signal, the same values as the fixed-predictor polynomials with one subtraction per order and no multiplies.

- **Pass A (analysis)**: `scan_range()` walks each candidate signal once (mono or each channel; for stereo, left, right, mid and side), summing `|e_o|` for orders 0-4 in one loop. A constant signal shows up as a zero order-1 sum. `estimate_rice_bits()` prices each order at the better of two Rice parameters, and the cheapest wins, capped at VERBATIM's size. For stereo, `choose_stereo_plan()` picks the cheapest of the four channel assignments, with side coded one bit deeper. The loop accumulates 32-bit partial sums, spilled at intervals proven overflow-free for the depth (`sum_chunk_samples()`); builds defining `MICRO_FLAC_CHECK_SCAN_SUMS` check that proof at run time.
- **Pass B (emission)**: `write_subframe()` writes each chosen signal's subframe in one walk, at Pass A's Rice parameter. There is no exact cost pass. Since `zigzag(r) <= 2|r|`, a subframe's exact size is at most Pass A's estimate plus half a bit per residual (`fixed_subframe_bits_bound()`). When that bound is below VERBATIM's size, FIXED is written and the bound proves the output capacity once for the unchecked `write_rice_block()`. Otherwise the residuals are priced exactly and VERBATIM is written unless FIXED is strictly smaller. Never writing a subframe larger than VERBATIM is what makes `get_max_output_bytes()` a hard bound.

### Rice Partitioning

With `FLACEncoderOptions::max_rice_partition_order`, a subframe's residuals may be split into 2^p partitions, each with its own Rice parameter (RFC 9639 §9.2.7). It is chosen after the predictor, in Pass B: one walk sums the residual magnitudes per leaf partition at the highest order allowed, and `choose_partitions()` prices every order from those sums, merging neighbors bottom-up. The size bound holds partition by partition. A subframe that does not split takes the single-partition code, so with partitioning off every other option runs unchanged.

### Wasted Bits

With `FLACEncoderOptions::wasted_bits`, a subframe whose samples all end in the same k zero bits is coded k bits shallower (RFC 9639 §9.2.2). Pass A finds k as the trailing zeros of the OR of each candidate's samples. Every residual of the shifted signal is the original's divided by 2^k, so the sums are rescaled rather than rescanned, and Pass B folds the shift into the unpacking. Mid is detected from its own samples: when left and right share k wasted bits, mid may keep only k - 1, because the decoder rebuilds mid's dropped low bit from side's. The unary count in the subframe header is the same for every coding of a signal, so the estimates leave it out and only the capacity checks add it.

### Compile-Time Switches

`MICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS` (Kconfig `MICRO_FLAC_ENCODER_ENABLE_RICE_PARTITIONS`, host CMake option of the same name) compiles partitioning out. `max_rice_partition_order` is then validated and ignored. It is a PUBLIC definition because `flac_encoder.h` reads it to report it (`FLACEncoder::RICE_PARTITIONS_AVAILABLE`).

### Bit Writer Design

`bit_writer.h` mirrors `bit_reader.h`: stack-local state (`BitWriterLocal`) the compiler can keep in registers, and header-only `FLAC_ALWAYS_INLINE` functions on it. `write_rice()` writes a whole code with one OR and shift when it fits the accumulator, and `write_zeros()` stores long unary runs as whole zero bytes. Residuals go through `write_rice_block()`, which skips every bounds check once the caller has proven the capacity, keeps the writer state in locals, and drains bytes only when the next code does not fit. Its output is identical to a `write_rice()` loop, which `test_bit_writer` checks.

CRC is not computed inside the writer. `write_frame_header()` runs `calculate_crc8()` over the header bytes it writes directly, and `close_frame()` runs `update_crc16()` once over the finished frame.

## Optimizations

### Bitstream Reading

The bit-stream primitives live in `bit_reader.h` as header-only `FLAC_ALWAYS_INLINE` functions operating on a `BitReaderLocal` stack struct. Hoisting bit-reader state into a local struct lets the compiler keep it in registers across hot loops, avoiding aliasing-induced spills through the decoder's member fields.

The decoder uses a platform-sized bit buffer: 64-bit on host/64-bit platforms (refilled 8 bytes at a time) and 32-bit on ESP32/32-bit platforms (refilled 4 bytes at a time). This avoids unnecessary 64-bit arithmetic on embedded targets while reducing refill frequency on desktop.

### LPC Accumulator Type Selection

The public `restore_lpc()` function dispatches to a specialized implementation based on overflow analysis and buffer type:

1. **Overflow Detection** (`can_use_lpc_32bit()`):
   - Analyzes sample depth, LPC coefficients, order, and quantization shift
   - Calculates maximum possible values before and after shift
   - Determines if 32-bit arithmetic will overflow

2. **32-bit Fast Path** (`restore_lpc_32bit()`):
   - Dispatched by `restore_lpc()` when overflow is impossible (most 16-bit audio)
   - ~2x faster on ESP32-S3
   - C++ implementation uses unrolled loops for orders 1-12
   - Xtensa assembly implementation with loop unrolling for orders 1-12

3. **64-bit Safe Path** (`restore_lpc_64bit()`):
   - Dispatched by `restore_lpc()` when 32-bit arithmetic may overflow
   - Used for high-resolution audio (e.g., 24-bit) with large coefficients
   - Prevents overflow in all valid FLAC streams
   - C++ implementation uses unrolled loops for orders 1-12
   - Xtensa assembly implementation uses MULL/MULSH instructions on ESP32 with loop unrolling for orders 1-12

4. **33-bit MID_SIDE Path** (`restore_lpc()` with `int64_t*` buffer):
   - Dispatched via the `int64_t*` overload of `restore_lpc()`
   - Used for the side channel in MID_SIDE stereo when the output sample depth is 32 bits
   - Side channel samples can be up to 33 bits wide, requiring `int64_t` buffers
   - Always uses 64-bit accumulation (no Xtensa assembly dispatch)

### Output

Output packing converts the internal planar 32-bit representation to interleaved PCM in the user's output buffer. The implementation is in `pcm_packing.cpp` with optimized fast paths dispatched by `write_samples()`:

**Native output mode** (bytes per sample matches bit depth):

- **16-bit mono**: Pointer-cast to `int16_t*`, 4-sample unrolled loop
- **16-bit stereo**: Pointer-cast to `int16_t*`, 4-sample unrolled loop
- **24-bit stereo**: Byte-by-byte little-endian packing, 2-sample unrolled loop
- **General fallback**: Handles all other bit depths and channel counts (non-byte-aligned with LSB zero-padding)

**32-bit output mode** (all samples left-justified to 32 bits):

- **32-bit mono**: Pointer-cast to `int32_t*`, shift in place
- **32-bit stereo**: Pointer-cast to `int32_t*`, shift-and-interleave
- **32-bit general**: Handles arbitrary channel counts

## References

- [FLAC Format Specification](https://xiph.org/flac/format.html)
- [Nayuki's Simple FLAC Implementation](https://www.nayuki.io/res/simple-flac-implementation/)
- [Mike Hansen's C++ FLAC decoder port](https://github.com/synesthesiam/flac-decoder)
- [Xtensa ISA Reference](https://www.cadence.com/content/dam/cadence-www/global/en_US/documents/tools/ip/tensilica-ip/isa-summary.pdf)
