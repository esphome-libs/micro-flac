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
  Full frame               1117.8     26.8x     1201.5     25.0x
  1000 byte chunks         1122.1     26.7x     1206.7     24.9x
  500 byte chunks          1127.6     26.6x     1212.8     24.7x
  100 byte chunks          1171.5     25.6x     1260.4     23.8x
  4 byte chunks            2473.3     12.1x     2642.9     11.4x
  1 byte chunks            6769.5      4.4x     7208.0      4.2x
```

### ESP32-S3 @ 240 MHz (24-bit/48 kHz stereo, 30 seconds, packed 24-bit output)

```text
================================================================
                     Benchmark Summary
================================================================

                                CRC Disabled           CRC Enabled
  Test Case              Time (ms) Real-time   Time (ms) Real-time
  --------------------  ---------- ---------  ---------- ---------
  Full frame               1622.7     18.5x     1810.0     16.6x
  1000 byte chunks         1633.2     18.4x     1819.6     16.5x
  500 byte chunks          1645.2     18.2x     1832.6     16.4x
  100 byte chunks          1740.0     17.2x     1935.9     15.5x
  4 byte chunks            4604.2      6.5x     4977.4      6.0x
  1 byte chunks           13553.6      2.2x    14439.6      2.1x
```

### ESP32-S3 @ 240 MHz (24-bit/48 kHz stereo, 30 seconds, 32-bit output)

```text
================================================================
                     Benchmark Summary
================================================================

                                CRC Disabled           CRC Enabled
  Test Case              Time (ms) Real-time   Time (ms) Real-time
  --------------------  ---------- ---------  ---------- ---------
  Full frame               1589.8     18.9x     1778.4     16.9x
  1000 byte chunks         1601.2     18.7x     1787.8     16.8x
  500 byte chunks          1613.2     18.6x     1800.9     16.7x
  100 byte chunks          1707.6     17.6x     1903.3     15.8x
  4 byte chunks            4555.4      6.6x     4928.5      6.1x
  1 byte chunks           13455.1      2.2x    14341.4      2.1x
```

Streaming with chunks of 100 bytes or larger has negligible overhead compared to full-frame decoding. CRC checking adds roughly 5-8% overhead for 16-bit and ~10-12% for 24-bit audio.

## Interpreting Results

### Real-Time Factor (RTF)

RTF = decode_time / audio_duration

- **RTF < 1.0**: Faster than real-time (good)
- **RTF = 1.0**: Exactly real-time
- **RTF > 1.0**: Slower than real-time (cannot stream)

### Expected Performance

| Device | Clock | Bit depth | Expected RTF | Real-time |
|--------|-------|-----------|--------------|-----------|
| ESP32 | 240 MHz | 16-bit | 0.12-0.14 | 7-8x |
| ESP32-S3 | 240 MHz | 16-bit | 0.037-0.040 | 25-27x |
| ESP32-S3 | 240 MHz | 24-bit | 0.054-0.061 | 16-19x |
| ESP32-P4 | 360 MHz | 16-bit | 0.042-0.044 | 23-24x |
| ESP32-P4 | 360 MHz | 24-bit | 0.055-0.061 | 16-18x |

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
