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
| ESP32 | 240 MHz | 16-bit | PSRAM | 0.105-0.130 | 8-10x |
| ESP32 | 240 MHz | 16-bit | Internal | 0.078-0.084 | 12-13x |
| ESP32-S3 | 240 MHz | 16-bit | PSRAM | 0.030-0.034 | 29-34x |
| ESP32-S3 | 240 MHz | 24-bit | PSRAM | 0.043-0.054 | 19-23x |
| ESP32-P4 | 360 MHz | 16-bit | PSRAM | 0.037-0.041 | 25-27x |
| ESP32-P4 | 360 MHz | 24-bit | PSRAM | 0.050-0.058 | 17-20x |

On the original ESP32, PSRAM access is much slower than internal SRAM, so placing the working buffer in internal memory (`CONFIG_MICRO_FLAC_PREFER_INTERNAL=y`) is roughly 30-35% faster. On the ESP32-S3, the same switch saves only ~2% (16-bit) to ~4% (24-bit), and on the ESP32-P4 it is below 1%. The S3/P4 numbers above are measured with the default PSRAM placement, and switching to internal SRAM yields essentially the same range.

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

### Very slow performance

- Verify CPU frequency is 240 MHz
- Check that optimization flags (-O2) are enabled
- Ensure you're not running in debug mode
