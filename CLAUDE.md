# microFLAC - Claude Development Guide

FLAC audio decoder and encoder library optimized for ESP32 embedded devices. The decoder supports both native FLAC and Ogg FLAC containers with automatic format detection; the encoder produces native FLAC from 4-24 bit PCM in 1-8 channels.

See `README.md` for full API docs, configuration options, and performance details.
See `src/README.md` for internal architecture, decoder/encoder pipelines, and LPC optimization details.

## Build Commands

### Host Build (Linux/macOS)

```bash
cd host_examples/flac_to_wav
cmake -B build && cmake --build build
./build/flac_to_wav input.flac output.wav       # native FLAC
./build/flac_to_wav input.oga output.wav        # Ogg FLAC

# With sanitizers (recommended for development)
cmake -DENABLE_SANITIZERS=ON -B build && cmake --build build
```

```bash
cd host_examples/wav_to_flac
cmake -B build && cmake --build build
./build/wav_to_flac input.wav output.flac

# With sanitizers (recommended for development)
cmake -DENABLE_SANITIZERS=ON -B build && cmake --build build
```

### ESP32 Build (PlatformIO)

```bash
cd examples/decode_benchmark
pio run -e esp32s3                          # Build only
pio run -e esp32s3 -t upload -t monitor     # Build, flash, and monitor
```

### Run Tests

```bash
cd host_examples/flac_to_wav
cmake -B build && cmake --build build
python3 test_flac_decoder.py                            # Run normal decode tests
python3 test_flac_decoder.py --mode all                 # Run all tests (slow: includes byte-by-byte streaming)
python3 test_flac_decoder.py --format flac              # Native FLAC only
python3 test_flac_decoder.py --format ogg               # Ogg FLAC only
python3 test_flac_decoder.py --mode streaming            # Streaming tests only
python3 test_flac_decoder.py --chunk-sizes 1,64,4096     # Custom chunk sizes
```

```bash
cd tests/encoder
cmake -B build && cmake --build build
./build/test_bit_writer                                 # Bit writer unit test (assert-based)
./build/test_encoder_config                             # Encoder config/argument validation unit test
```

```bash
cd host_examples/wav_to_flac
cmake -B build && cmake --build build
python3 test_flac_encoder.py                            # Encoder round-trip/reference test suite
python3 test_flac_encoder.py --block-sizes 16,192,4096   # Custom block sizes
python3 test_flac_encoder.py --skip-real-audio           # Synthetic sources only, no test corpus decode
python3 test_flac_encoder.py --skip-depth-channel        # Skip the 8/24-bit and 3-8 channel matrix
python3 test_flac_encoder.py --skip-partitions           # Skip the Rice partitioning pass
```

## Key Architecture Decisions

1. **Namespace**: All code in `micro_flac` namespace
2. **Unified decode() API**: Single `decode()` method handles container detection, header parsing, and frame decoding via an internal state machine
3. **Container auto-detection**: Detects native FLAC ("fLaC") vs Ogg FLAC ("OggS") from the first 4 bytes
4. **Streaming design**: All parsing supports incremental byte-by-byte processing
5. **Modular CMake**: Separate files for sources (`cmake/sources.cmake`), ESP-IDF config (`cmake/esp-idf.cmake`), and host config (`cmake/host.cmake`)
6. **Platform-aware**: Automatic Xtensa assembly selection on ESP32/ESP32-S3
7. **C++14 standard**: Modern C++ with minimal STL usage (`std::vector` for optional metadata storage, `std::unique_ptr` for owned resources)
8. **FLACEncoder never buffers input or allocates**: configured from a `PcmFormat` and `FLACEncoderOptions` by the constructor or `reset(format, options)` (both go through `configure()`, which clears every derived field first so a rejected config leaves fresh-encoder state); neither fails, and the next call reports config errors. `encode()` consumes exactly one block and writes one frame, or consumes nothing. Pass A picks each subframe's fixed predictor and the stereo assignment; Pass B writes subframes, trying Rice partitioning there, and writes nothing larger than VERBATIM. Every format takes one path: the input is unpacked 256 samples per channel at a time into the object's 2 KB `chunk_`. Single-signal walks read through `PackedSignal`, deriving mid/side straight from the packed input (`unpack_mid()`/`unpack_side()`); stereo Pass A unpacks left/right (`unpack_stereo()`) and derives mid/side in place. See `src/README.md`
9. **Files are named for the FLAC concept they handle**: a concept both directions touch keeps both in one file pair (`frame_header` parses and writes, `pcm_packing` packs and unpacks); `flac_format.h` holds every RFC 9639 constant (except the CRC tables in `crc.cpp` and the few the public headers expose). Class files hold their own pipelines

## Common Tasks

### Adding a source file

1. Add `.cpp` or `.S` file to `src/`
2. Update `cmake/sources.cmake` (add to `FLAC_SOURCES` or `FLAC_XTENSA_SOURCES`)
3. Rebuild

### Adding a Kconfig option

1. Add option to `Kconfig`
2. Map to compile definition in `cmake/esp-idf.cmake`
3. Use in code with `#ifdef MICRO_FLAC_NEW_FEATURE`

### Testing changes

1. **Host tests first** (faster iteration):

   ```bash
   cd host_examples/flac_to_wav
   cmake -DENABLE_SANITIZERS=ON -B build && cmake --build build
   python3 test_flac_decoder.py                    # Normal decode tests (fast)
   python3 test_flac_decoder.py --mode all          # All tests including streaming (slow)
   ```

2. **ESP32 performance test**:

   ```bash
   cd examples/decode_benchmark
   pio run -e esp32s3 -t upload -t monitor
   ```

3. **Encoder host tests**:

   ```bash
   cd host_examples/wav_to_flac
   cmake -DENABLE_SANITIZERS=ON -B build && cmake --build build
   python3 test_flac_encoder.py     # Round-trip/reference test suite

   cd ../../tests/encoder           # Unit tests (sanitizers on by default)
   cmake -B build && cmake --build build
   ./build/test_bit_writer          # Bit writer unit test
   ./build/test_encoder_config      # Encoder config/argument validation unit test

   # The encoder's compile-time switches: test_encoder_config adapts to each
   # (options for a feature compiled out must be ignored)
   cmake -DMICRO_FLAC_ENCODER_ENABLE_RICE_PARTITIONS=OFF -B build-min
   cmake --build build-min && ./build-min/test_encoder_config
   ```

### Debugging decode issues

1. Verify CRC checking is enabled (enabled by default): `decoder.is_crc_check_enabled()`
2. Check result codes: negative = error, see `flac_decoder.h` for full enum
3. Use host build with sanitizers to catch memory issues
4. Compare output to ffmpeg: `ffmpeg -i input.flac -f s16le output.raw`

### Debugging encode issues

1. Check result codes: negative = error, see `flac_encoder.h` for full enum
2. Use host build with sanitizers to catch memory issues
3. Round-trip through the decoder (`flac_to_wav`) or `ffmpeg`/`flac -t` to confirm the output is valid FLAC
4. Verify the output buffer is at least `encoder.get_max_output_bytes()`

## Things to Watch Out For

- **Metadata before decode**: Call `set_max_metadata_size()` BEFORE first `decode()` call
- **Buffer allocation**: Allocate output buffer after receiving `FLAC_DECODER_HEADER_READY`, using stream info to compute size
- **bytes_consumed tracking**: Always advance your input pointer by `bytes_consumed` after each `decode()` call
- **Error codes**: Negative return values are errors, positive are informational (SUCCESS=0, HEADER_READY=1, END_OF_STREAM=2, NEED_MORE_DATA=3)
- **32-bit output mode**: Use the `int32_t*` overload of `decode()` for 32-bit left-justified output
- **Xtensa assembly**: Only available on ESP32/ESP32-S3 (LX6/LX7), not RISC-V chips
- **No seeking support**: Decoder is designed for streaming, not random access
- **Encoder output buffer**: `encoder.get_max_output_bytes()` is constant until `reset(format, options)` reconfigures the encoder and fits the header and every frame; the check happens up front and the call fails cleanly with `FLAC_ENCODER_ERROR_OUTPUT_BUFFER_TOO_SMALL` if the buffer is too small
- **Encoder input**: packed bytes per `PcmFormat` (interleaved, little-endian, signed, `ceil(bps/8)` bytes per sample, odd depths left-justified), exactly the decoder's `uint8_t*` output. 2-byte samples must be 2-byte aligned. Advance the input by `bytes_consumed`
- **Encoder tests reach private members**: `test_encoder_config` is built with `-fno-access-control`; keep test-only hooks out of `flac_encoder.h` and set private fields from the test instead
- **Encoder compile-time switch**: `MICRO_FLAC_ENCODER_DISABLE_RICE_PARTITIONS` (Kconfig `MICRO_FLAC_ENCODER_ENABLE_RICE_PARTITIONS`, host CMake option of the same name) is a PUBLIC definition because `flac_encoder.h` reads it. Build both settings (with `-DENABLE_WERROR=ON`) after touching guarded code; unused-function/constant warnings show up only in the reduced build. A build without partitioning must run every other option's code unchanged: only a subframe that actually splits takes the partitioned walks
- **Wasted bits stay outside the size estimates**: a subframe's unary wasted-bits count is the same for every coding of its signal, so every estimate and bound omits it and only the capacity checks (`capacity_for(bw, bound + signal.wasted_bits())`) add it. Pass A's analysis is rescaled from the unshifted scan (`finish_wasted_analysis()`), never rescanned
- **Encoder hot path**: the scan, cascade, unpack and Rice loops are tuned against Xtensa's 14 usable registers. After touching `scan_range()`, `write_subframe()`, the cascade primitives, `PackedSignal::unpack()` or the unpack loops in `pcm_packing.cpp`, diff the Xtensa disassembly of those functions before and after: a hot loop must not gain stack references or instructions. Frame setup (`start_frame()`, `finish_frame()`, `close_frame()`) and `write_subframe()` stay out of line. Xtensa's `l32i` reaches only 1020 bytes past its base: a stack frame that grows beyond that, or an object field placed after the 2 KB `chunk_` (hence its place in `FLACEncoder`'s member order), costs an extra instruction per access
- **Encoder output must not change by accident**: refactors should produce byte-identical output in both switch builds; hash a set of encodes before and after
