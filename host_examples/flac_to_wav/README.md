# FLAC to WAV Converter Example

Converts FLAC files to WAV format using the microFLAC decoder. Supports native FLAC and Ogg FLAC containers, all bit depths (8, 12, 16, 20, 24, 32 bits), and various channel configurations (mono to 8 channels).

## Building

### Prerequisites

- CMake 3.16 or later
- A C++14 compatible compiler (gcc, clang, etc.)
- Make or Ninja build system

### Build Steps

```bash
# From the flac_to_wav directory
cmake -B build
cmake --build build
```

### Install

After building, install `flac_to_wav` to `/usr/local/bin`:

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
./build/flac_to_wav <input.flac> <output.wav>
```

### Example

```bash
./build/flac_to_wav song.flac song.wav
```

The program displays stream info (sample rate, channels, bit depth, total samples, metadata blocks) and performs MD5 verification of the decoded audio against the signature in the FLAC header.

```text
=== MD5 Verification ===
Expected MD5: ac3c581ce17991866b0dcdea3b9dfd43
Computed MD5: ac3c581ce17991866b0dcdea3b9dfd43
Result: PASS - MD5 signatures match!
```

## Output Format

The converter produces standard WAV files:

- **8-bit, 16-bit**: PCM format (format code 1)
- **12-bit, 20-bit, 24-bit, 32-bit**: WAVE_FORMAT_EXTENSIBLE (format code 0xFFFE)
- **Byte order**: Little-endian (standard for WAV)
- **Bit alignment**: Non-byte-aligned bit depths are zero-padded in the LSB

## Testing

A comprehensive test suite is available to validate the decoder against the official FLAC test files. See [TESTING.md](TESTING.md) for details on setting up test files, running the test suite, and interpreting results.
