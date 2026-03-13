# microFLAC - Claude Development Guide

FLAC audio decoder library optimized for ESP32 embedded devices. Supports both native FLAC and Ogg FLAC containers with automatic format detection.

See `README.md` for full API docs, configuration options, and performance details.
See `src/README.md` for internal architecture, decoder pipeline, and LPC optimization details.

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

## Key Architecture Decisions

1. **Namespace**: All code in `micro_flac` namespace
2. **Unified decode() API**: Single `decode()` method handles container detection, header parsing, and frame decoding via an internal state machine
3. **Container auto-detection**: Detects native FLAC ("fLaC") vs Ogg FLAC ("OggS") from the first 4 bytes
4. **Streaming design**: All parsing supports incremental byte-by-byte processing
5. **Modular CMake**: Separate files for sources (`cmake/sources.cmake`), ESP-IDF config (`cmake/esp-idf.cmake`), and host config (`cmake/host.cmake`)
6. **Platform-aware**: Automatic Xtensa assembly selection on ESP32/ESP32-S3
7. **C++14 standard**: Modern C++ with minimal STL usage (`std::vector` for optional metadata storage, `std::unique_ptr` for owned resources)

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

### Debugging decode issues

1. Verify CRC checking is enabled (enabled by default): `decoder.is_crc_check_enabled()`
2. Check result codes: negative = error, see `flac_decoder.h` for full enum
3. Use host build with sanitizers to catch memory issues
4. Compare output to ffmpeg: `ffmpeg -i input.flac -f s16le output.raw`

## Things to Watch Out For

- **Metadata before decode**: Call `set_max_metadata_size()` BEFORE first `decode()` call
- **Buffer allocation**: Allocate output buffer after receiving `FLAC_DECODER_HEADER_READY`, using stream info to compute size
- **bytes_consumed tracking**: Always advance your input pointer by `bytes_consumed` after each `decode()` call
- **Error codes**: Negative return values are errors, positive are informational (SUCCESS=0, HEADER_READY=1, END_OF_STREAM=2, NEED_MORE_DATA=3)
- **32-bit output mode**: Use the `int32_t*` overload of `decode()` for 32-bit left-justified output
- **Xtensa assembly**: Only available on ESP32/ESP32-S3 (LX6/LX7), not RISC-V chips
- **No seeking support**: Decoder is designed for streaming, not random access
