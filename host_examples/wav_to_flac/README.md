# WAV to FLAC Converter Example

Converts PCM WAV files to native FLAC using the microFLAC encoder. Supports 8, 16 and 24-bit input in 1-8 channels. Uses fixed predictors (orders 0-4), optionally linear prediction (`--lpc`), and a fixed block size.

## Building

### Prerequisites

- CMake 3.16 or later
- A C++14 compatible compiler (gcc, clang, etc.)
- Make or Ninja build system

### Build Steps

```bash
# From the wav_to_flac directory
cmake -B build
cmake --build build
```

### Install

After building, install `wav_to_flac` to `/usr/local/bin`:

```bash
sudo cmake --install build
```

To install to a custom location:

```bash
cmake --install build --prefix ~/.local
```

### Build with Sanitizers

```bash
cmake -B build -DENABLE_SANITIZERS=ON
cmake --build build
```

## Usage

```bash
./build/wav_to_flac [--block-size N] [--lpc] [--lpc-order N] [--lpc-stereo-search] [--partition-order N] [--no-wasted-bits] <input.wav> <output.flac>
```

### Flags

| Flag | Description |
| ---- | ----------- |
| `--block-size N` | Samples per channel in every frame but the last, 16-65535 (default: 4096) |
| `--lpc` | Also try linear prediction (LPC) on every subframe, at the default maximum order of 8 |
| `--lpc-order N` | Highest LPC order tried, 1-12. Implies `--lpc` |
| `--lpc-stereo-search` | Design LPC for all four stereo candidates (left, right, mid, side) before choosing the channel assignment. Implies `--lpc` |
| `--partition-order N` | Largest Rice partition order tried, 0-6 (default 0, one partition per subframe) |
| `--no-wasted-bits` | Turn off wasted-bits detection (`FLACEncoderOptions::wasted_bits`) |

### Example

```bash
./build/wav_to_flac song.wav song.flac
./build/wav_to_flac --lpc song.wav song.flac   # about 7-9% smaller on typical music
./build/wav_to_flac --lpc-stereo-search --partition-order 6 song.wav song.flac   # within 0.01% of flac -5
```

WAV data is already in the encoder's packed byte layout, so the program reads one block at a time straight into `encode()` and passes the short remainder to `finish()`. It then seeks back and rewrites the stream header, so STREAMINFO carries the total sample count, the frame size range and the MD5 signature. The library leaves the MD5 to its caller; the program computes it with the `md5.h` it shares with `flac_to_wav`. It prints encode stats and, for stereo input, how many frames used each channel assignment.

```text
Done.
Frames written: 76
Samples per channel encoded: 308700
Output file: song.flac
MD5 signature: d220a9bf831451e7a1b4db4454323368
Output bytes: 589401
Bits per sample (average, all channels): 7.637
Largest frame: 10701 bytes (max_output_bytes bound: 17430 bytes)
Compression ratio vs 16-bit PCM: 2.095
Channel assignment counts: independent=25 left/side=1 right/side=7 mid/side=43
```

## Input Format

Uncompressed PCM WAV is accepted:

- **Format**: PCM (format code 1) or WAVE_FORMAT_EXTENSIBLE (format code 0xFFFE) wrapping PCM
- **Bit depth**: 8, 16 or 24-bit. 8-bit WAV data is unsigned with a 128 bias (RIFF's one exception to signed samples), so its sign bit is flipped on the way in; 16 and 24-bit are signed
- **Channels**: 1 to 8
- **Byte order**: Little-endian (standard for WAV)
- **Data size**: a data chunk that claims more than the file holds (a WAV streamed to a pipe declares 0xFFFFFFFF) is read to the end of the file

## Testing

`test_flac_encoder.py` encodes synthetic and real-audio WAV sources across block sizes, bit depths, channel counts and encoder options, decodes the result with `flac_to_wav` (and `ffmpeg`/`flac` when available), and checks for bit-exact PCM. See the root [README.md](../../README.md#testing) for the command.

The unit tests in `tests/encoder` cover what the round trip cannot reach: `test_bit_writer` covers `src/bit_writer.h`, and `test_encoder_config` covers every call's result codes, the finished stream header, depths WAV cannot carry (4-24 bits) and internal state such as the frame-number limit.
