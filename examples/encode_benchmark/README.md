# FLAC Encode Benchmark for ESP32

Measures FLAC encoding performance on ESP32-S3 and ESP32-P4. Decodes the embedded test clip to PCM once at startup, then re-encodes it repeatedly across a set of test cases (block sizes, LPC orders, Rice partitioning, 24-bit and mono) to benchmark encode throughput and the compression achieved.

## Features

- Real music as the test signal, decoded from an embedded FLAC clip at startup (no filesystem required)
- 16-bit stereo at block sizes 4096, 1152, 576 and 192
- LPC (`FLACEncoderOptions::lpc`) at orders 4, 8 and 12, and with `lpc_stereo_search`
- Rice partitioning up to order 6 (`max_rice_partition_order`, the "r6" cases)
- 24-bit stereo and mono (the same clip widened to 24 bits), with and without LPC
- Per-frame timing with min/max/avg/stddev
- Real-Time Factor (RTF) and realtime multiple per test case
- Achieved bytes/sample, bits/sample, and compression ratio vs. the input PCM
- Channel assignment histogram (independent / left-side / right-side / mid-side)
- Combined summary table for easy comparison

## Test Signal

The benchmark reuses `test_audio_flac.h` from the `decode_benchmark` example (a 16-bit/48 kHz clip of public domain music; see that example's README for how to regenerate it) through `INCLUDE_DIRS` in `main/CMakeLists.txt`. At startup it decodes the clip with `FLACDecoder` into an interleaved PCM buffer, which every encode pass then reads.

## Building and Running

### PlatformIO

```bash
# Build and flash for ESP32-S3
pio run -e esp32s3 -t upload -t monitor

# Build and flash for ESP32-P4
pio run -e esp32p4 -t upload -t monitor
```

### ESP-IDF

```bash
idf.py set-target esp32s3   # or esp32p4
idf.py build
idf.py flash monitor
```

When switching targets in an existing build directory, run `idf.py fullclean` (or delete `sdkconfig`) first.

## Expected Output

The benchmark runs each test case in turn and prints a combined summary table at the end. The 16-bit cases encode the 30-second 48 kHz stereo clip; the 24-bit cases encode the same clip widened to 24 bits. Each case encodes the clip 8 times: Time (ms) is the total across all 8 passes (240 s of audio), while Real-time is per pass.

### ESP32-S3 @ 240 MHz

```text
================================================================
                     Benchmark Summary
================================================================

  Test Case              Time (ms)     Real-time   Bytes/smp       Ratio
  --------------------  ----------  ------------  ----------  ----------
  4096 (large)            11266.40         21.3x       0.879      2.276x
  1152 (typical)          11494.50         20.9x       0.878      2.277x
  576 (small)             11801.32         20.3x       0.887      2.255x
  192 (tiny)              12855.32         18.7x       0.925      2.163x
  LPC-4 4096              21249.30         11.3x       0.838      2.386x
  LPC-8 4096              26477.51          9.0x       0.836      2.392x
  LPC-8 1152              25892.37          9.3x       0.841      2.379x
  LPC-12 4096             29648.38          8.1x       0.836      2.392x
  LPC-8 search 4096       37174.39          6.4x       0.833      2.401x
  LPC-8 search 1152       36336.99          6.6x       0.838      2.387x
  4096 r6                 13401.68         17.9x       0.874      2.290x
  1152 r6                 14417.87         16.6x       0.878      2.278x
  LPC-8 r6 4096           29176.26          8.2x       0.830      2.411x
  LPC-8 search r6 4096    40374.28          5.9x       0.828      2.417x
  24b 4096 (large)        15047.86         15.9x       1.880      1.596x
  24b 1152 (typical)      15276.88         15.7x       1.879      1.596x
  24b 4096 r6             17707.02         13.5x       1.874      1.601x
  24b LPC-8 4096          57844.81          4.1x       1.836      1.634x
  24b mono 4096            6045.64         39.6x       1.882      1.594x
```

### ESP32-P4 @ 360 MHz

Measured on a pre-rev.3.0 eval board, which the benchmark runs at 360 MHz; rev 3.0 and later chips run at a different clock, so their times will differ.

```text
================================================================
                     Benchmark Summary
================================================================

  Test Case              Time (ms)     Real-time   Bytes/smp       Ratio
  --------------------  ----------  ------------  ----------  ----------
  4096 (large)             9295.55         25.8x       0.879      2.276x
  1152 (typical)           9468.07         25.3x       0.878      2.277x
  576 (small)              9693.32         24.8x       0.887      2.255x
  192 (tiny)              10498.04         22.9x       0.925      2.163x
  LPC-4 4096              16480.25         14.5x       0.838      2.386x
  LPC-8 4096              19473.99         12.3x       0.836      2.392x
  LPC-8 1152              19413.04         12.4x       0.841      2.379x
  LPC-12 4096             21566.25         11.1x       0.836      2.392x
  LPC-8 search 4096       25479.56          9.4x       0.833      2.401x
  LPC-8 search 1152       25437.49          9.4x       0.838      2.387x
  4096 r6                 11170.45         21.5x       0.874      2.290x
  1152 r6                 12049.54         19.9x       0.878      2.278x
  LPC-8 r6 4096           21744.74         11.0x       0.830      2.411x
  LPC-8 search r6 4096    28114.12          8.5x       0.828      2.417x
  24b 4096 (large)        11906.92         20.1x       1.880      1.596x
  24b 1152 (typical)      12079.09         19.9x       1.879      1.596x
  24b 4096 r6             14102.36         17.0x       1.874      1.601x
  24b LPC-8 4096          35525.71          6.7x       1.836      1.634x
  24b mono 4096            4676.46         51.2x       1.882      1.594x
```

The encoder's output is platform-independent, so the Bytes/smp and Ratio columns are identical on every device; only the times differ.

## Interpreting Results

### Real-Time Factor (RTF)

RTF = encode_time / audio_duration, per pass (Time (ms) / 8 / 30 s)

- **RTF < 1.0**: Faster than real-time; the encoder keeps up with live input
- **RTF = 1.0**: Exactly real-time
- **RTF > 1.0**: Slower than real-time (cannot keep up with live input)

### Expected Performance

| Device | Clock | Test cases | Expected RTF | Real-time |
|--------|-------|------------|--------------|-----------|
| ESP32-S3 | 240 MHz | 16-bit no LPC, blocks 192-4096 | 0.047-0.054 | 19-21x |
| ESP32-S3 | 240 MHz | 16-bit LPC-8, blocks 1152/4096 | 0.108-0.111 | 9x |
| ESP32-S3 | 240 MHz | 24-bit no LPC, blocks 1152/4096 | 0.063-0.064 | 16x |
| ESP32-P4 | 360 MHz | 16-bit no LPC, blocks 192-4096 | 0.039-0.044 | 23-26x |
| ESP32-P4 | 360 MHz | 16-bit LPC-8, blocks 1152/4096 | 0.081 | 12x |
| ESP32-P4 | 360 MHz | 24-bit no LPC, blocks 1152/4096 | 0.050 | 20x |

The P4's 1.5x clock advantage translates into about 1.2x the S3's throughput on the 16-bit cases without LPC, 1.3-1.5x on 16-bit LPC (the stereo search gains the most), and 1.6x on 24-bit LPC-8.

### Bytes/Sample and Compression Ratio

`Bytes/sample` is the average FLAC output size per PCM sample across all channels; multiply by 8 for bits/sample. `Ratio` is `(bits_per_sample / 8) / bytes_per_sample`, i.e. how much smaller the FLAC output is than the input PCM (2 bytes per sample for the 16-bit cases, 3 for the 24-bit ones).

Performance and compression both vary with:

- Block size (larger blocks amortize per-frame overhead)
- Signal content (tonal material compresses better than noise)
- Stereo channel assignment (side subframes are cheaper when channels correlate)

## Configuration

### sdkconfig.defaults

- Custom partition table (`partitions_4mb.csv`) with a ~4 MB app partition for the embedded test audio
- Watchdogs disabled (for uninterrupted, accurately-timed benchmark runs)
- PSRAM enabled (for boards with PSRAM)
- Main task stack: 10KB
- Log level: WARN (reduced overhead)
- Compiler optimization: performance

### Target-specific files

- `sdkconfig.defaults.esp32s3`: 240 MHz CPU, 16 MB flash, octal PSRAM at 80 MHz, 64 KB data cache with 64-byte lines, 32 KB instruction cache
- `sdkconfig.defaults.esp32p4`: 360 MHz CPU (pre-rev.3.0 eval board), 16 MB flash, hex PSRAM at 200 MHz

## Memory Usage

The decoded test signal (30 seconds of 48 kHz stereo 16-bit PCM, about 5.5 MB) is allocated from PSRAM, falling back to any available RAM. A 24-bit copy (about 8.2 MB) also goes in PSRAM; without room for it, the 24-bit cases are skipped. The encoder reads them sequentially, so PSRAM's latency has little effect. The full run needs about 14 MB of free PSRAM, such as a 16 MB S3 module or the P4 eval board's 32 MB; with 8 MB only the 16-bit cases run. The output buffer, sized by `FLACEncoder::get_max_output_bytes()`, uses the default heap.

## File Structure

```text
encode_benchmark/
├── main/
│   ├── CMakeLists.txt         # ESP-IDF component file (also pulls in decode_benchmark/main for test_audio_flac.h)
│   └── main.cpp                # Benchmark code (test clip decode + encode timing)
├── CMakeLists.txt              # ESP-IDF project file
├── partitions_4mb.csv          # Partition table with a ~4 MB app partition
├── platformio.ini              # PlatformIO configuration
├── sdkconfig.defaults          # ESP-IDF settings
├── sdkconfig.defaults.esp32s3  # ESP32-S3 specific settings
├── sdkconfig.defaults.esp32p4  # ESP32-P4 specific settings
└── README.md                   # This file
```

## Troubleshooting

### "failed to allocate ... byte PCM buffer" / "ERROR: failed to decode embedded test audio"

- The board has neither enough PSRAM nor enough internal RAM free for the decoded clip; regenerate `decode_benchmark/main/test_audio_flac.h` with a shorter clip (see that example's README)

### Very slow performance

- Verify CPU frequency is 240 MHz (ESP32-S3) or 360 MHz (ESP32-P4)
- Check that optimization flags (`-O2`) are enabled
- Ensure you're not running in debug mode
