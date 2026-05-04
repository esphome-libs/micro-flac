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
   python convert_flac.py -i clip.flac -o src/test_audio_flac.h -v test_audio_flac_data
   python convert_flac.py -i clip_24bit.flac -o src/test_audio_flac_24bit.h -v test_audio_flac_24bit_data
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
  Full frame                918.26     32.7x      991.73     30.3x
  1000 byte chunks          922.81     32.5x      997.24     30.1x
  500 byte chunks           928.47     32.3x     1003.55     29.9x
  100 byte chunks           975.27     30.8x     1053.93     28.5x
  4 byte chunks            2373.16     12.6x     2527.86     11.9x
  1 byte chunks            6935.53      4.3x     7296.88      4.1x
```

### ESP32-S3 @ 240 MHz (24-bit/48 kHz stereo, 30 seconds, packed 24-bit output)

```text
================================================================
                     Benchmark Summary
================================================================

                                CRC Disabled           CRC Enabled
  Test Case              Time (ms) Real-time   Time (ms) Real-time
  --------------------  ---------- ---------  ---------- ---------
  Full frame               1385.14     21.7x     1550.19     19.4x
  1000 byte chunks         1396.60     21.5x     1560.53     19.2x
  500 byte chunks          1409.14     21.3x     1574.06     19.1x
  100 byte chunks          1510.16     19.9x     1682.95     17.8x
  4 byte chunks            4580.11      6.6x     4919.77      6.1x
  1 byte chunks           14542.14      2.1x    15336.69      2.0x
```

### ESP32-S3 @ 240 MHz (24-bit/48 kHz stereo, 30 seconds, 32-bit output)

```text
================================================================
                     Benchmark Summary
================================================================

                                CRC Disabled           CRC Enabled
  Test Case              Time (ms) Real-time   Time (ms) Real-time
  --------------------  ---------- ---------  ---------- ---------
  Full frame               1364.75     22.0x     1531.08     19.6x
  1000 byte chunks         1376.54     21.8x     1541.01     19.5x
  500 byte chunks          1389.18     21.6x     1554.69     19.3x
  100 byte chunks          1489.55     20.1x     1662.72     18.0x
  4 byte chunks            4538.67      6.6x     4878.53      6.1x
  1 byte chunks           14435.03      2.1x    15229.60      2.0x
```

Streaming with chunks of 100 bytes or larger has negligible overhead compared to full-frame decoding. CRC checking adds roughly ~8% overhead for 16-bit and ~12% for 24-bit audio.

## Interpreting Results

### Real-Time Factor (RTF)

RTF = decode_time / audio_duration

- **RTF < 1.0**: Faster than real-time (good)
- **RTF = 1.0**: Exactly real-time
- **RTF > 1.0**: Slower than real-time (cannot stream)

### Expected Performance

| Device | Clock | Bit depth | Working buffer | Expected RTF | Real-time |
|--------|-------|-----------|----------------|--------------|-----------|
| ESP32 | 240 MHz | 16-bit | PSRAM | 0.107-0.131 | 7-9x |
| ESP32 | 240 MHz | 16-bit | Internal | 0.079-0.087 | 11-13x |
| ESP32-S3 | 240 MHz | 16-bit | PSRAM | 0.031-0.035 | 28-33x |
| ESP32-S3 | 240 MHz | 24-bit | PSRAM | 0.046-0.056 | 18-22x |
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
├── src/
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
