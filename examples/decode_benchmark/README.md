# FLAC Decode Benchmark for ESP32

Measures FLAC decoding performance on ESP32 devices. Runs multiple test cases with varying input chunk sizes to benchmark both full-frame and streaming decode paths, with and without CRC checking.

## Features

- Embedded FLAC data (no filesystem required)
- Tests multiple streaming chunk sizes (full frame, 1000, 500, 100, 4, and 1 byte)
- Runs each test case with CRC disabled and CRC enabled
- Per-frame timing with min/max/avg/stddev
- Combined summary table for easy comparison

## Audio Source Preparation

The benchmark requires FLAC audio data embedded in a C header file. A placeholder file is included, but you need to generate the real one with actual audio.

### Recommended Source

Use public domain music from [Musopen on Archive.org](https://archive.org/details/MusopenCollectionAsFlac):

- Beethoven Symphony No. 3 "Eroica" - Czech National Symphony Orchestra
- Same source used by the micro-opus benchmark

### Steps

1. **Download a FLAC file** from Musopen or another public domain source

2. **Extract a 10-30 second clip** using ffmpeg:

   16-bit/48 kHz:

   ```bash
   ffmpeg -i input.flac -ss 60 -t 30 -c:a flac -ar 48000 -sample_fmt s16 clip.flac
   ```

   24-bit/48 kHz:

   ```bash
   ffmpeg -i input.flac -ss 60 -t 30 -c:a flac -ar 48000 -sample_fmt s32 clip_24bit.flac
   ```

   Options:
   - `-ss 60`: Start at 60 seconds into the file
   - `-t 30`: Extract 30 seconds of audio
   - `-ar 48000`: Sample rate 48 kHz
   - `-sample_fmt s16`: 16-bit samples (`s32` for 24-bit)

3. **Generate the C header files**:

   ```bash
   python convert_flac.py -i clip.flac -o main/test_audio_flac.h -v test_audio_flac_data
   python convert_flac.py -i clip_24bit.flac -o main/test_audio_flac_24bit.h -v test_audio_flac_24bit_data
   ```

## Building and Running

### PlatformIO

```bash
# Build and flash for ESP32
pio run -e esp32 -t upload -t monitor

# Build and flash for ESP32-S3
pio run -e esp32s3 -t upload -t monitor
pio run -e esp32s3_24bit -t upload -t monitor

# Build and flash for ESP32-P4
pio run -e esp32p4 -t upload -t monitor
pio run -e esp32p4_24bit -t upload -t monitor
```

### ESP-IDF

```bash
idf.py set-target esp32   # or esp32s3
idf.py build
idf.py flash monitor
```

## Expected Output

The benchmark runs each chunk size first with CRC disabled, then with CRC enabled, and prints a combined summary table at the end.

### ESP32-S3 @ 240 MHz (16-bit/48 kHz stereo, 30 seconds)

```text
================================================================
                     Benchmark Summary
================================================================

                                CRC Disabled           CRC Enabled
  Test Case              Time (ms) Real-time   Time (ms) Real-time
  --------------------  ---------- ---------  ---------- ---------
  Full frame                888.33     33.8x      967.05     31.0x
  1000 byte chunks          892.87     33.6x      971.50     30.9x
  500 byte chunks           899.25     33.4x      978.39     30.7x
  100 byte chunks           947.54     31.7x     1030.15     29.1x
  4 byte chunks            2394.07     12.5x     2551.77     11.8x
  1 byte chunks            7110.94      4.2x     7474.91      4.0x
```

### ESP32-S3 @ 240 MHz (24-bit/48 kHz stereo, 30 seconds, packed 24-bit output)

```text
================================================================
                     Benchmark Summary
================================================================

                                CRC Disabled           CRC Enabled
  Test Case              Time (ms) Real-time   Time (ms) Real-time
  --------------------  ---------- ---------  ---------- ---------
  Full frame               1313.19     22.8x     1479.07     20.3x
  1000 byte chunks         1325.30     22.6x     1489.87     20.1x
  500 byte chunks          1338.41     22.4x     1504.00     19.9x
  100 byte chunks          1442.87     20.8x     1616.57     18.6x
  4 byte chunks            4620.40      6.5x     4960.62      6.0x
  1 byte chunks           14919.41      2.0x    15714.71      1.9x
```

### ESP32-S3 @ 240 MHz (24-bit/48 kHz stereo, 30 seconds, 32-bit output)

```text
================================================================
                     Benchmark Summary
================================================================

                                CRC Disabled           CRC Enabled
  Test Case              Time (ms) Real-time   Time (ms) Real-time
  --------------------  ---------- ---------  ---------- ---------
  Full frame               1293.20     23.2x     1460.13     20.5x
  1000 byte chunks         1305.50     23.0x     1470.59     20.4x
  500 byte chunks          1318.67     22.8x     1484.61     20.2x
  100 byte chunks          1422.31     21.1x     1596.29     18.8x
  4 byte chunks            4573.71      6.6x     4913.94      6.1x
  1 byte chunks           14790.53      2.0x    15585.75      1.9x
```

Streaming in chunks of 500 bytes or larger adds under 2% over full-frame decoding (100-byte chunks: 6-10%). CRC checking adds roughly 9% for 16-bit and 12% for 24-bit audio.

## Interpreting Results

### Real-Time Factor (RTF)

RTF = decode_time / audio_duration

- **RTF < 1.0**: Faster than real-time
- **RTF = 1.0**: Exactly real-time
- **RTF > 1.0**: Slower than real-time (cannot stream)

### Expected Performance

| Device | Clock | Bit depth | Working buffer | Expected RTF | Real-time |
|--------|-------|-----------|----------------|--------------|-----------|
| ESP32 | 240 MHz | 16-bit | PSRAM | 0.071-0.086 | 12-14x |
| ESP32 | 240 MHz | 16-bit | Internal | 0.038-0.043 | 23-26x |
| ESP32-S3 | 240 MHz | 16-bit | PSRAM | 0.030-0.034 | 29-34x |
| ESP32-S3 | 240 MHz | 24-bit | PSRAM | 0.043-0.054 | 19-23x |
| ESP32-P4 | 360 MHz | 16-bit | PSRAM | 0.037-0.041 | 25-27x |
| ESP32-P4 | 360 MHz | 24-bit | PSRAM | 0.050-0.058 | 17-20x |

On the original ESP32, PSRAM access is much slower than internal SRAM, so placing the working buffer in internal memory (`CONFIG_MICRO_FLAC_PREFER_INTERNAL=y`) is roughly 2x faster. The ESP32 rows run flash and PSRAM at 80 MHz; see [ESP32 flash and PSRAM clocks](#esp32-flash-and-psram-clocks). On the ESP32-S3, the same switch saves only ~2% (16-bit) to ~4% (24-bit), and on the ESP32-P4 it is below 1%. The S3/P4 numbers above are measured with the default PSRAM placement, and switching to internal SRAM yields essentially the same range.

Performance varies based on:

- FLAC compression level
- Block size
- Number of channels
- Bit depth
- Streaming chunk size (significant below ~100 bytes)

## Configuration

### sdkconfig.defaults

- CPU frequency: 240 MHz
- Watchdogs disabled (for accurate timing)
- PSRAM enabled (for boards with PSRAM)
- Main task stack: 8KB
- Log level: WARN (reduced overhead)

### ESP32 flash and PSRAM clocks

The `esp32` environment runs flash at 80 MHz in DIO mode (`CONFIG_ESPTOOLPY_FLASHFREQ_80M` in `sdkconfig.defaults.esp32`, `board_build.f_flash` in `platformio.ini`), and PSRAM at 80 MHz. ESP-IDF only offers 80 MHz PSRAM with 80 MHz flash: at 40 MHz flash, `CONFIG_SPIRAM_SPEED_80M` silently falls back to 40 MHz.

Measured on an ESP32 (rev v1.0), 16-bit clip, full frame, decode speed as a multiple of real time (`1 / RTF`), CRC off / on:

| Working buffer | DIO 40 MHz    | DIO 80 MHz (default here) | QIO 80 MHz    |
| -------------- | ------------- | ------------------------- | ------------- |
| Internal       | 22.8x / 20.4x | 26.5x / 23.6x             | 28.6x / 25.3x |
| PSRAM          | 9.6x / 7.8x   | 14.1x / 11.7x             | 15.1x / 12.7x |

Flash speed matters here mostly because the benchmark reads its FLAC input from flash: a slice copied to internal RAM first decodes at the same speed under all three settings. With the working buffer in PSRAM, the 80 MHz PSRAM clock helps either way.

Caveats before using these settings in an application:

- The module and its flash chip must be rated for 80 MHz at the operating temperature ([ESP-IDF speed guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/performance/speed.html)). Espressif's common modules are; marginal third-party boards may not be.
- Some ESP32-WROVER-B and -E batches run flash at 80 MHz but PSRAM only at 40 MHz, and fail at boot with "PSRAM chip not found or not supported" ([forum report](https://esp32.com/viewtopic.php?p=86792)). Set `CONFIG_SPIRAM_SPEED_40M=y` on those boards.
- QIO is another 6-7% faster but needs the flash's WP/HD pins wired for quad mode, which DIO does not. In a QIO build `CONFIG_ESPTOOLPY_FLASHMODE` still reads "dio" (the bootloader switches modes); the boot log's `SPI Mode : QIO` confirms it.
- With some 32 Mbit PSRAM chips at 80 MHz, ESP-IDF uses an SPI peripheral (HSPI or VSPI, `CONFIG_SPIRAM_OCCUPY_*_HOST`) to output the PSRAM clock, so the application cannot use it.
- An existing generated `sdkconfig.esp32` keeps its old clock settings: delete it once to pick up these defaults.

## File Structure

```text
decode_benchmark/
├── main/
│   ├── CMakeLists.txt         # ESP-IDF component file
│   ├── main.cpp               # Benchmark code
│   ├── test_audio_flac.h      # Generated 16-bit FLAC data header
│   └── test_audio_flac_24bit.h # Generated 24-bit FLAC data header
├── CMakeLists.txt             # ESP-IDF project file
├── convert_flac.py            # FLAC to C header converter
├── platformio.ini             # PlatformIO configuration
├── sdkconfig.defaults         # ESP-IDF settings
└── README.md                  # This file
```

## Troubleshooting

### "Failed to read FLAC header"

- Ensure you've generated the header file with valid FLAC data
- Check that the FLAC file is not corrupted

### "Failed to allocate output buffer"

- The ESP32 may not have enough RAM
- Try a smaller FLAC clip or reduce block size

### "PSRAM chip not found" on the ESP32

- The board's PSRAM may not run at 80 MHz (see [ESP32 flash and PSRAM clocks](#esp32-flash-and-psram-clocks)): set `CONFIG_SPIRAM_SPEED_40M=y` in `sdkconfig.defaults.esp32` and delete the generated `sdkconfig.esp32`

### Very slow performance

- Verify CPU frequency is 240 MHz
- Check that optimization flags (-O2) are enabled
- Ensure you're not running in debug mode
