#!/usr/bin/env python3
"""
FLAC encoder round-trip and reference test suite

Drives wav_to_flac (the encoder CLI) over a matrix of synthetic and real-audio
WAV sources and block-size configurations, then verifies each encoded file:

  1. Our decoder (flac_to_wav) round-trips it bit-exactly, and verifies the
     MD5 signature wav_to_flac wrote into STREAMINFO.
  2. ffmpeg decodes it bit-exactly (skipped with a note if ffmpeg is missing).
  3. `flac -t` accepts it, MD5 signature included (skipped with a note if the
     flac CLI is missing).
  4. Encoded size is smaller than the source WAV for tonal (compressible)
     sources.
  5. wav_to_flac's largest frame never exceeded the encoder's own
     get_max_output_bytes() bound.
  6. The finished STREAMINFO wav_to_flac writes back over the provisional
     header carries the right total sample count, a fixed block size, and
     plausible minimum/maximum frame sizes.

The stereo-estimation sanity check (L == R input must compress far better
than forced independent coding) lives in test_encoder_config, which can reach
the encoder's private switch for it.

Synthetic WAVs and encoded/decoded intermediates are generated into
test_results/ (gitignored) using fixed RNG seeds, so re-running is
deterministic and leaves no stray state between runs.
"""

import argparse
import array
import json
import math
import random
import re
import struct
import subprocess
import sys
import wave
from collections import namedtuple
from datetime import datetime
from pathlib import Path

# ==============================================================================
# Configuration
# ==============================================================================

SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_WAV_TO_FLAC = SCRIPT_DIR / "build" / "wav_to_flac"
DEFAULT_FLAC_TO_WAV = SCRIPT_DIR / ".." / "flac_to_wav" / "build" / "flac_to_wav"

# What flac_to_wav prints when a stream's MD5 signature verifies, and what
# `flac -t` prints when a stream has none.
FLAC_TO_WAV_MD5_PASS = "PASS - MD5 signatures match"
FLAC_MD5_UNSET_WARNING = "cannot check MD5 signature"
TEST_FILES_DIR = SCRIPT_DIR / ".." / ".." / "test" / "flac-test-files" / "subset"

RESULTS_DIR = SCRIPT_DIR / "test_results"
SYNTH_WAV_DIR = RESULTS_DIR / "synthetic_wavs"
REAL_WAV_DIR = RESULTS_DIR / "real_wavs"
DEPTH_WAV_DIR = RESULTS_DIR / "depth_channel_wavs"
ENCODED_DIR = RESULTS_DIR / "encoded"
DECODED_DIR = RESULTS_DIR / "decoded"

SAMPLE_RATE = 44100
DEFAULT_ENCODER_BLOCK_SIZE = 4096  # wav_to_flac's default --block-size
DEFAULT_BLOCK_SIZES = [16, 192, 256, 1000, 4096, 4608]

RNG_SEED = 20260721  # fixed, for reproducibility across runs

# Below this block size, per-frame overhead (frame header + CRC-16 + FIXED
# subframe warmup samples, all re-paid every frame) can legitimately exceed
# the savings from Rice coding a mere handful of residuals, especially for
# broadband content like a chirp. Block size 16 is in the matrix to exercise
# the block-size-code table's 8-bit-escape path, not to demonstrate
# compression, so the compression-sanity check (checks per case, #4) is
# skipped below this size.
MIN_BLOCK_SIZE_FOR_COMPRESSION_CHECK = 192

# Files from the IETF FLAC conformance corpus used as "real audio" sources:
# the blocksize-variant + fixed-order files in test/flac-test-files/subset
# (44.1 kHz 16-bit stereo), plus two 24-bit ones.
REAL_AUDIO_FILES = [
    "01 - blocksize 4096.flac",
    "02 - blocksize 4608.flac",
    "04 - blocksize 192.flac",
    "06 - blocksize 512.flac",
    "08 - blocksize 1000.flac",
    "17 - all fixed orders.flac",
    # 24-bit: real high-resolution music, whose residuals need Rice parameters
    # above 14 (5-bit parameters, coding method 01), and a mono file built to
    # push predictors toward overflow
    "28 - high resolution audio, default settings.flac",
    "63 - predictor overflow check, 24-bit.flac",
]

BOUND_RE = re.compile(
    r"Largest frame:\s*(\d+)\s*bytes\s*\(max_output_bytes bound:\s*(\d+)\s*bytes\)"
)

# Filled in by main() after argument parsing.
WAV_TO_FLAC = None
FLAC_TO_WAV = None
HAVE_FFMPEG = False
HAVE_FLAC = False

SourceSpec = namedtuple(
    "SourceSpec", ["name", "channels", "tonal", "wav_path", "bits"], defaults=(16,)
)


# ==============================================================================
# Small helpers
# ==============================================================================

def run_command(cmd, timeout=120):
    """Run a shell command and return (success, stdout, stderr)"""
    try:
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
        return result.returncode == 0, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return False, "", "Command timed out"
    except Exception as e:  # pylint: disable=broad-except
        return False, "", str(e)


def safe_name(s):
    """Turn an arbitrary string into a filesystem-safe basename component"""
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", s)


def extract_pcm_from_wav(wav_file):
    """Extract raw PCM data from WAV file (skip headers), mirroring
    test_flac_decoder.py's helper of the same name."""
    try:
        with open(wav_file, "rb") as f:
            riff = f.read(4)
            if riff != b"RIFF":
                return None
            f.read(4)  # file size
            wave_id = f.read(4)
            if wave_id != b"WAVE":
                return None
            while True:
                chunk_id = f.read(4)
                if not chunk_id:
                    return None
                chunk_size = int.from_bytes(f.read(4), "little")
                if chunk_id == b"data":
                    return f.read(chunk_size)
                f.seek(chunk_size + chunk_size % 2, 1)  # RIFF pads odd chunks
    except Exception as e:  # pylint: disable=broad-except
        print(f"Error reading WAV file {wav_file}: {e}")
        return None


def clamp16(v):
    if v > 32767:
        return 32767
    if v < -32768:
        return -32768
    return v


def write_wav(path, num_channels, sample_rate, samples):
    """samples: array.array('h') of interleaved int16 PCM"""
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(num_channels)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(samples.tobytes())


def write_wav_depth(path, num_channels, sample_rate, bits, samples):
    """Write interleaved PCM at 8, 16 or 24 bits.

    samples is a sequence of Python ints already in the signed range for
    `bits`. 8-bit WAV PCM is unsigned with a 128 bias (RIFF's one exception to
    signed samples), so it is re-centered on the way out; 16- and 24-bit are
    little-endian signed."""
    width = bits // 8
    raw = bytearray()
    if bits == 8:
        for v in samples:
            raw.append((v + 128) & 0xFF)
    elif bits == 16:
        for v in samples:
            raw += struct.pack("<h", v)
    elif bits == 24:
        for v in samples:
            u = v & 0xFFFFFF
            raw += bytes((u & 0xFF, (u >> 8) & 0xFF, (u >> 16) & 0xFF))
    else:
        raise ValueError(f"unsupported bit depth {bits}")

    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(num_channels)
        wf.setsampwidth(width)
        wf.setframerate(sample_rate)
        wf.writeframes(bytes(raw))


# ==============================================================================
# Synthetic signal generators (stdlib only, fixed seeds for reproducibility)
# ==============================================================================

def gen_silence(n, channels):
    return array.array("h", [0]) * (n * channels)


def gen_dc(n, channels, value):
    return array.array("h", [clamp16(value)]) * (n * channels)


def gen_tone_mono(n, freq, amplitude, sample_rate=SAMPLE_RATE):
    out = array.array("h")
    for i in range(n):
        out.append(clamp16(int(round(amplitude * math.sin(2 * math.pi * freq * i / sample_rate)))))
    return out


def gen_tone_stereo_equal(n, freq, amplitude, sample_rate=SAMPLE_RATE):
    """Identical L/R tone -- side channel collapses to a constant zero."""
    out = array.array("h")
    for i in range(n):
        v = clamp16(int(round(amplitude * math.sin(2 * math.pi * freq * i / sample_rate))))
        out.append(v)
        out.append(v)
    return out


def gen_tone_stereo_generic(n, freq_l, amp_l, freq_r, amp_r, sample_rate=SAMPLE_RATE):
    """L and R at different frequencies/amplitudes -- a "generic" stereo signal,
    neither hard-panned nor identical."""
    out = array.array("h")
    for i in range(n):
        l = clamp16(int(round(amp_l * math.sin(2 * math.pi * freq_l * i / sample_rate))))
        r = clamp16(int(round(amp_r * math.sin(2 * math.pi * freq_r * i / sample_rate))))
        out.append(l)
        out.append(r)
    return out


def gen_tone_stereo_hard_panned(n, freq, amplitude, sample_rate=SAMPLE_RATE):
    """Full signal on L, silence on R."""
    out = array.array("h")
    for i in range(n):
        v = clamp16(int(round(amplitude * math.sin(2 * math.pi * freq * i / sample_rate))))
        out.append(v)
        out.append(0)
    return out


def gen_chirp(n, f0, f1, amplitude, channels, sample_rate=SAMPLE_RATE):
    """Linear frequency sweep from f0 to f1 Hz over the full duration."""
    duration = n / sample_rate
    out = array.array("h")
    for i in range(n):
        t = i / sample_rate
        phase = 2 * math.pi * (f0 * t + (f1 - f0) * t * t / (2 * duration))
        v = clamp16(int(round(amplitude * math.sin(phase))))
        for _ in range(channels):
            out.append(v)
    return out


def gen_noise(n, channels, seed, amplitude=32000):
    rng = random.Random(seed)
    out = array.array("h")
    for _ in range(n * channels):
        out.append(rng.randint(-amplitude, amplitude))
    return out


def gen_square(n, freq, amplitude, channels, sample_rate=SAMPLE_RATE):
    out = array.array("h")
    for i in range(n):
        half_period_index = int(i * 2 * freq / sample_rate)
        v = amplitude if half_period_index % 2 == 0 else -amplitude
        for _ in range(channels):
            out.append(v)
    return out


# ==============================================================================
# Source construction
# ==============================================================================

def build_synthetic_sources(out_dir):
    """Generate the synthetic WAV test matrix (silence, DC, sine, chirp,
    noise, square, L==R stereo, hard-panned stereo -- mono+stereo where
    sensible) into out_dir and return their SourceSpecs."""
    out_dir.mkdir(parents=True, exist_ok=True)
    sources = []
    one_sec = SAMPLE_RATE
    two_sec = SAMPLE_RATE * 2

    def add(name, channels, tonal, samples):
        path = out_dir / f"{name}.wav"
        write_wav(path, channels, SAMPLE_RATE, samples)
        sources.append(SourceSpec(name, channels, tonal, path))

    # Silence -> CONSTANT
    add("silence_mono", 1, True, gen_silence(one_sec, 1))
    add("silence_stereo", 2, True, gen_silence(one_sec, 2))

    # DC offset -> CONSTANT
    add("dc_mono", 1, True, gen_dc(one_sec, 1, 5000))
    add("dc_stereo", 2, True, gen_dc(one_sec, 2, -8000))

    # Pure sine -> FIXED
    add("sine_mono", 1, True, gen_tone_mono(one_sec, 440.0, 16000))
    add("sine_stereo", 2, True, gen_tone_stereo_generic(one_sec, 440.0, 16000, 550.0, 12000))

    # Chirp -> FIXED
    add("chirp_mono", 1, True, gen_chirp(two_sec, 200.0, 8000.0, 16000, 1))
    add("chirp_stereo", 2, True, gen_chirp(two_sec, 200.0, 8000.0, 16000, 2))

    # White noise -> VERBATIM-heavy (no compression guarantee)
    add("noise_mono", 1, False, gen_noise(one_sec, 1, RNG_SEED + 1))
    add("noise_stereo", 2, False, gen_noise(one_sec, 2, RNG_SEED + 2))

    # Square wave
    add("square_mono", 1, True, gen_square(one_sec, 300.0, 18000, 1))
    add("square_stereo", 2, True, gen_square(one_sec, 300.0, 18000, 2))

    # Stereo decorrelation edge cases
    add("lr_equal_stereo", 2, True, gen_tone_stereo_equal(one_sec, 523.0, 18000))
    add("hard_panned_stereo", 2, False, gen_tone_stereo_hard_panned(one_sec, 440.0, 20000))

    return sources


def gen_multichannel(n, channels, bits, seed, tonal, sample_rate=SAMPLE_RATE):
    """Interleaved PCM at an arbitrary depth and channel count.

    Each channel gets its own frequency and amplitude so no two are
    correlated, which keeps the multichannel cases from degenerating into
    something a single-channel test would already cover. `tonal` selects a
    compressible sine (plus a small deterministic dither so it is not a pure
    CONSTANT/low-order signal) versus incompressible white noise."""
    peak = (1 << (bits - 1)) - 1
    rng = random.Random(seed)
    out = []
    for i in range(n):
        for c in range(channels):
            if tonal:
                freq = 220.0 * (c + 1)
                amp = peak * (0.7 - 0.05 * c)
                v = int(round(amp * math.sin(2 * math.pi * freq * i / sample_rate)))
                v += ((i * (c + 7)) % 11) - 5
            else:
                v = rng.randint(-peak, peak)
            out.append(max(-peak - 1, min(peak, v)))
    return out


def build_depth_channel_sources(out_dir):
    """Bit-depth and channel-count coverage beyond 16-bit mono and stereo:
    8-bit (1-byte samples), 24-bit (3-byte samples, and the shortest Pass A
    spill intervals), and channel counts from 3 to the format's maximum of 8."""
    out_dir.mkdir(parents=True, exist_ok=True)
    sources = []
    n = SAMPLE_RATE // 2  # half a second keeps the 8-channel cases quick

    def add(name, channels, bits, tonal, samples):
        path = out_dir / f"{name}.wav"
        write_wav_depth(path, channels, SAMPLE_RATE, bits, samples)
        sources.append(SourceSpec(name, channels, tonal, path, bits))

    # Depths below 16: 1-byte packed samples.
    for bits in (8,):
        for ch in (1, 2):
            add(f"depth{bits}_{ch}ch", ch, bits, True,
                gen_multichannel(n, ch, bits, RNG_SEED + 10 + ch, True))

    # Depths above 16: 3-byte packed samples and Pass A's shortest spill
    # intervals (32 samples, 16 for the 25-bit side channel).
    for bits in (24,):
        for ch in (1, 2):
            add(f"depth{bits}_{ch}ch", ch, bits, True,
                gen_multichannel(n, ch, bits, RNG_SEED + 20 + ch, True))
        add(f"depth{bits}_2ch_noise", 2, bits, False,
            gen_multichannel(n, 2, bits, RNG_SEED + 29, False))

    # Channel counts above stereo, at both containers. 3 and 8 bracket the
    # range; 6 is the common surround case.
    for ch in (3, 6, 8):
        add(f"multi{ch}ch_16b", ch, 16, True,
            gen_multichannel(n, ch, 16, RNG_SEED + 30 + ch, True))
        add(f"multi{ch}ch_24b", ch, 24, True,
            gen_multichannel(n, ch, 24, RNG_SEED + 40 + ch, True))

    # Incompressible multichannel -> every subframe should fall back to
    # VERBATIM, which is what actually backs get_max_output_bytes()'s bound.
    add("multi6ch_24b_noise", 6, 24, False,
        gen_multichannel(n, 6, 24, RNG_SEED + 50, False))

    # Constant signals at a wide depth (CONSTANT subframes at bps 24/25).
    add("multi4ch_24b_silence", 4, 24, True, [0] * (n * 4))

    # Wasted bits (FLACEncoderOptions::wasted_bits): shallower audio carried
    # in a deeper container, so every sample of a channel ends in the same
    # zero bits -- 16-bit stereo in 24 bits (mid then codes one bit wider
    # than side's shift, see encode_frame()), 20-bit mono in 24, 12-bit
    # stereo and 13-bit mono in 16, 7-bit stereo in 8, and six channels
    # shifted by 0-5 bits each. The reference decoders check the subframe
    # headers' wasted-bits counts.
    def shifted(samples, channels, shifts):
        return [v << shifts[i % channels] for i, v in enumerate(samples)]

    add("depth24_2ch_wasted8", 2, 24, True,
        shifted(gen_multichannel(n, 2, 16, RNG_SEED + 60, True), 2, [8, 8]))
    add("depth24_1ch_wasted4", 1, 24, True,
        shifted(gen_multichannel(n, 1, 20, RNG_SEED + 61, True), 1, [4]))
    add("depth16_2ch_wasted4", 2, 16, True,
        shifted(gen_multichannel(n, 2, 12, RNG_SEED + 64, True), 2, [4, 4]))
    add("depth16_1ch_wasted3", 1, 16, True,
        shifted(gen_multichannel(n, 1, 13, RNG_SEED + 65, True), 1, [3]))
    add("depth8_2ch_wasted1", 2, 8, True,
        shifted(gen_multichannel(n, 2, 7, RNG_SEED + 62, True), 2, [1, 1]))
    add("multi6ch_24b_wasted", 6, 24, True,
        shifted(gen_multichannel(n, 6, 19, RNG_SEED + 63, True), 6, [0, 1, 2, 3, 4, 5]))

    return sources


def build_short_tail_sources(out_dir):
    """Final-frame edge cases at the default 4096 block size: a length that
    leaves a final frame under 16 samples (4096*3 + 7), mono and stereo, and
    one that ends exactly on a block boundary (4096*3), so finish() gets no
    remainder and writes no frame."""
    out_dir.mkdir(parents=True, exist_ok=True)
    n = 4096 * 3 + 7
    sources = []

    def add(name, channels, samples):
        path = out_dir / f"{name}.wav"
        write_wav(path, channels, SAMPLE_RATE, samples)
        sources.append(SourceSpec(name, channels, True, path))

    add("short_tail_mono", 1, gen_tone_mono(n, 440.0, 15000))
    add("short_tail_stereo", 2, gen_tone_stereo_generic(n, 440.0, 15000, 660.0, 11000))
    add("exact_blocks_stereo", 2, gen_tone_stereo_generic(4096 * 3, 440.0, 15000, 660.0, 11000))

    return sources


def build_real_audio_sources(out_dir):
    """Decode a handful of 16 and 24-bit mono/stereo files from the IETF FLAC
    conformance corpus to WAV via ffmpeg. Returns (sources, skip_notes)."""
    sources = []
    skipped = []

    if not TEST_FILES_DIR.exists():
        skipped.append(f"test corpus not found at {TEST_FILES_DIR} -- skipping real-audio sources")
        return sources, skipped

    if not HAVE_FFMPEG:
        skipped.append("ffmpeg not available -- skipping real-audio sources (need it to make WAVs)")
        return sources, skipped

    out_dir.mkdir(parents=True, exist_ok=True)

    for fname in REAL_AUDIO_FILES:
        flac_path = TEST_FILES_DIR / fname
        if not flac_path.exists():
            skipped.append(f"{fname}: not found in corpus")
            continue

        probe_cmd = (
            f'ffprobe -v error -select_streams a:0 '
            f'-show_entries stream=channels,bits_per_raw_sample '
            f'-of default=noprint_wrappers=1:nokey=1 "{flac_path}"'
        )
        ok, out, _ = run_command(probe_cmd, timeout=15)
        channels, bit_depth = None, None
        if ok:
            lines = out.strip().splitlines()
            if len(lines) >= 2:
                try:
                    channels = int(lines[0])
                    bit_depth = int(lines[1])
                except ValueError:
                    pass

        if channels not in (1, 2) or bit_depth not in (16, 24):
            skipped.append(
                f"{fname}: unsupported channels={channels} bit_depth={bit_depth} "
                "(need 16 or 24-bit mono/stereo)"
            )
            continue

        base = safe_name(Path(fname).stem)
        wav_path = out_dir / f"{base}.wav"
        codec = "pcm_s24le" if bit_depth == 24 else "pcm_s16le"
        cmd = f'ffmpeg -y -i "{flac_path}" -c:a {codec} -f wav "{wav_path}" 2>&1'
        ok, out, _ = run_command(cmd, timeout=60)
        if not ok:
            skipped.append(f"{fname}: ffmpeg decode to WAV failed")
            continue

        sources.append(SourceSpec(f"real_{base}", channels, False, wav_path, bit_depth))

    return sources, skipped


# ==============================================================================
# Case execution
# ==============================================================================

class CaseResult:
    def __init__(self, name, source_name):
        self.name = name
        self.source_name = source_name
        self.args_desc = ""
        self.encode_ok = None
        self.encode_error = None
        self.bound_actual = None
        self.bound_limit = None
        self.bound_ok = None
        self.streaminfo_problem = None  # None = OK
        self.our_decode_ok = None
        self.our_pcm_match = None
        self.our_md5_ok = None
        self.ffmpeg_pcm_match = None  # None = not checked (ffmpeg missing)
        self.flac_test_ok = None      # None = not checked (flac missing)
        self.compression_checked = False
        self.compression_ok = None
        self.passed = None
        self.message = ""


def wav_frame_count(wav_path):
    """Sample frames (samples per channel) in a WAV file. Walks the RIFF
    chunks itself: ffmpeg writes 24-bit WAVs as WAVE_FORMAT_EXTENSIBLE, which
    the `wave` module reads only from Python 3.12."""
    with open(wav_path, "rb") as f:
        if f.read(4) != b"RIFF":
            raise ValueError(f"{wav_path}: not a RIFF file")
        f.read(4)  # file size
        if f.read(4) != b"WAVE":
            raise ValueError(f"{wav_path}: not a WAVE file")
        block_align = None
        while True:
            header = f.read(8)
            if len(header) < 8:
                raise ValueError(f"{wav_path}: no fmt/data chunks")
            chunk_id = header[:4]
            chunk_size = int.from_bytes(header[4:], "little")
            if chunk_id == b"fmt ":
                fmt = f.read(chunk_size)
                block_align = int.from_bytes(fmt[12:14], "little")
                if block_align == 0:
                    raise ValueError(f"{wav_path}: bad fmt chunk")
                f.seek(chunk_size % 2, 1)
            elif chunk_id == b"data":
                if block_align is None:
                    raise ValueError(f"{wav_path}: data chunk before fmt")
                return chunk_size // block_align
            else:
                f.seek(chunk_size + chunk_size % 2, 1)


def check_streaminfo(flac_path, source, block_size, largest_frame):
    """Parse the finished STREAMINFO wav_to_flac wrote over the provisional
    header. `largest_frame` is the largest frame wav_to_flac reported writing,
    which the maximum frame size must equal. Returns None if it is right, else
    a description of the problem."""
    try:
        data = flac_path.read_bytes()[:42]
    except OSError:
        return "could not read encoded file"
    if len(data) < 42 or data[:4] != b"fLaC":
        return "missing fLaC header"
    si = data[8:]
    min_block, max_block = struct.unpack(">HH", si[0:4])
    min_frame = int.from_bytes(si[4:7], "big")
    max_frame = int.from_bytes(si[7:10], "big")
    total_samples = ((si[13] & 0x0F) << 32) | int.from_bytes(si[14:18], "big")
    expected_samples = wav_frame_count(source.wav_path)

    if min_block != block_size or max_block != block_size:
        return f"block size {min_block}/{max_block}, expected {block_size}/{block_size}"
    if total_samples != expected_samples:
        return f"total samples {total_samples}, expected {expected_samples}"
    if not (0 < min_frame <= max_frame):
        return f"frame sizes min={min_frame} max={max_frame} not finalized"
    if largest_frame is not None and max_frame != largest_frame:
        return f"max frame size {max_frame}, but the largest frame written was {largest_frame}"
    return None


def run_case(source, extra_args, tag, block_size=None):
    """Encode `source` with wav_to_flac using `extra_args`, then run every
    applicable check against the result. `block_size` is the literal
    --block-size value passed, or None when wav_to_flac's default was used."""
    name = f"{source.name}__{tag}"
    result = CaseResult(name, source.name)
    result.args_desc = " ".join(extra_args) if extra_args else "(default)"

    flac_path = ENCODED_DIR / f"{safe_name(name)}.flac"
    args_str = (" " + " ".join(extra_args)) if extra_args else ""
    cmd = f'"{WAV_TO_FLAC}"{args_str} "{source.wav_path}" "{flac_path}"'
    ok, out, err = run_command(cmd, timeout=180)
    result.encode_ok = ok
    if not ok:
        result.encode_error = (err or out).strip()
        result.passed = False
        result.message = f"FAIL - encode failed: {result.encode_error[:150]}"
        return result

    m = BOUND_RE.search(out)
    if m:
        result.bound_actual = int(m.group(1))
        result.bound_limit = int(m.group(2))
        result.bound_ok = result.bound_actual <= result.bound_limit

    result.streaminfo_problem = check_streaminfo(
        flac_path,
        source,
        block_size if block_size is not None else DEFAULT_ENCODER_BLOCK_SIZE,
        result.bound_actual,
    )

    # Check 1: our decoder round-trip (always required)
    dec_wav = DECODED_DIR / f"{safe_name(name)}_dec.wav"
    cmd = f'"{FLAC_TO_WAV}" "{flac_path}" "{dec_wav}"'
    ok, out, err = run_command(cmd, timeout=180)
    result.our_decode_ok = ok
    if ok:
        src_pcm = extract_pcm_from_wav(source.wav_path)
        dec_pcm = extract_pcm_from_wav(dec_wav)
        result.our_pcm_match = src_pcm is not None and src_pcm == dec_pcm
        # flac_to_wav reports its MD5 verdict but exits 0 either way, and
        # skips the check for an all-zero (unset) signature.
        result.our_md5_ok = FLAC_TO_WAV_MD5_PASS in out
    else:
        result.our_pcm_match = False
        result.our_md5_ok = False

    # Check 2: ffmpeg round-trip. The raw format must match the source WAV's
    # own sample layout for the byte comparison below to be meaningful: 8-bit
    # WAV data is unsigned, 16/24-bit are little-endian signed.
    if HAVE_FFMPEG:
        raw_fmt = {8: ("pcm_u8", "u8"), 16: ("pcm_s16le", "s16le"), 24: ("pcm_s24le", "s24le")}[
            source.bits
        ]
        raw_out = DECODED_DIR / f"{safe_name(name)}_ffmpeg.raw"
        cmd = (
            f'ffmpeg -y -i "{flac_path}" -c:a {raw_fmt[0]} -f {raw_fmt[1]} "{raw_out}" 2>&1'
        )
        ok, out, err = run_command(cmd, timeout=180)
        if ok:
            src_pcm = extract_pcm_from_wav(source.wav_path)
            try:
                raw_bytes = raw_out.read_bytes()
            except OSError:
                raw_bytes = None
            result.ffmpeg_pcm_match = (
                src_pcm is not None and raw_bytes is not None and src_pcm == raw_bytes
            )
        else:
            result.ffmpeg_pcm_match = False

    # Check 3: flac -t, which fails on an MD5 mismatch and only warns when
    # the signature is unset -- so the warning counts as a failure too.
    if HAVE_FLAC:
        cmd = f'flac -t "{flac_path}" 2>&1'
        ok, out, err = run_command(cmd, timeout=180)
        result.flac_test_ok = ok and FLAC_MD5_UNSET_WARNING not in (out + err)

    # Check 4: compression sanity for tonal sources (skipped at pathologically
    # small block sizes -- see MIN_BLOCK_SIZE_FOR_COMPRESSION_CHECK)
    if source.tonal and (block_size is None or block_size >= MIN_BLOCK_SIZE_FOR_COMPRESSION_CHECK):
        result.compression_checked = True
        try:
            wav_size = source.wav_path.stat().st_size
            flac_size = flac_path.stat().st_size
            result.compression_ok = flac_size < wav_size
        except OSError:
            result.compression_ok = False

    # Aggregate
    problems = []
    if not result.our_pcm_match:
        problems.append("our-decoder PCM mismatch")
    elif not result.our_md5_ok:
        problems.append("our-decoder MD5 check did not pass")
    if result.bound_ok is False:
        problems.append(f"max_output_bytes bound violated ({result.bound_actual} > {result.bound_limit})")
    elif result.bound_ok is None:
        problems.append("could not parse the frame bound line from wav_to_flac output")
    if result.streaminfo_problem:
        problems.append(f"STREAMINFO: {result.streaminfo_problem}")
    if HAVE_FFMPEG and result.ffmpeg_pcm_match is False:
        problems.append("ffmpeg PCM mismatch")
    if HAVE_FLAC and result.flac_test_ok is False:
        problems.append("flac -t failed")
    if result.compression_checked and result.compression_ok is False:
        problems.append("compression sanity failed (encoded size >= WAV size)")

    result.passed = len(problems) == 0
    result.message = "PASS" if result.passed else "FAIL - " + "; ".join(problems)
    return result


# ==============================================================================
# Report generation
# ==============================================================================

def generate_report(all_results, skip_notes):
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    stats = {
        "total": len(all_results),
        "passed": sum(1 for r in all_results if r.passed is True),
        "failed": sum(1 for r in all_results if r.passed is False),
    }

    lines = [
        "=" * 80,
        "FLAC Encoder Test Report",
        f"Generated: {timestamp}",
        "=" * 80,
        "",
        "SUMMARY",
        "-" * 40,
        f"Total cases: {stats['total']}",
        f"Passed: {stats['passed']} ({stats['passed'] * 100 // stats['total'] if stats['total'] else 0}%)",
        f"Failed: {stats['failed']}",
        "",
    ]

    if skip_notes:
        lines.append("SKIPPED / NOTES")
        lines.append("-" * 40)
        for note in skip_notes:
            lines.append(f"  - {note}")
        lines.append("")

    lines.append("DETAILS")
    lines.append("-" * 40)
    for r in all_results:
        status = "PASS" if r.passed is True else "FAIL"
        lines.append(f"{status} {r.name} [{r.args_desc}]: {r.message}")

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    report_text = "\n".join(lines)
    report_file = RESULTS_DIR / "test_report.txt"
    with open(report_file, "w") as f:
        f.write(report_text)

    json_report = {
        "timestamp": timestamp,
        "summary": stats,
        "skip_notes": skip_notes,
        "results": [
            {
                "name": r.name,
                "source": r.source_name,
                "args": r.args_desc,
                "passed": r.passed,
                "message": r.message,
                "our_pcm_match": r.our_pcm_match,
                "ffmpeg_pcm_match": r.ffmpeg_pcm_match,
                "our_md5_ok": r.our_md5_ok,
                "flac_test_ok": r.flac_test_ok,
                "bound_actual": r.bound_actual,
                "bound_limit": r.bound_limit,
                "streaminfo_problem": r.streaminfo_problem,
                "compression_checked": r.compression_checked,
                "compression_ok": r.compression_ok,
            }
            for r in all_results
        ],
    }
    json_file = RESULTS_DIR / "test_report.json"
    with open(json_file, "w") as f:
        json.dump(json_report, f, indent=2)

    return report_text, report_file, json_file


# ==============================================================================
# Main
# ==============================================================================

def main():
    global WAV_TO_FLAC, FLAC_TO_WAV, HAVE_FFMPEG, HAVE_FLAC  # noqa: PLW0603

    parser = argparse.ArgumentParser(description="FLAC Encoder Test Suite")
    parser.add_argument(
        "--wav-to-flac-build-dir",
        type=str,
        default=None,
        help="Path to the build dir containing the wav_to_flac binary",
    )
    parser.add_argument(
        "--flac-to-wav-build-dir",
        type=str,
        default=None,
        help="Path to the build dir containing the flac_to_wav binary",
    )
    parser.add_argument(
        "--block-sizes",
        type=str,
        default=None,
        help="Comma-separated block sizes to test (default: 16,192,256,1000,4096,4608)",
    )
    parser.add_argument(
        "--skip-real-audio",
        action="store_true",
        help="Skip decoding the real-audio test corpus into WAV sources",
    )
    parser.add_argument(
        "--skip-depth-channel",
        action="store_true",
        help="Skip the bit-depth (8/24-bit) and channel-count (3-8) matrix",
    )
    parser.add_argument(
        "--skip-partitions",
        action="store_true",
        help="Skip the Rice partitioning (--partition-order) pass",
    )
    args = parser.parse_args()

    WAV_TO_FLAC = (
        Path(args.wav_to_flac_build_dir).resolve() / "wav_to_flac"
        if args.wav_to_flac_build_dir
        else DEFAULT_WAV_TO_FLAC
    )
    FLAC_TO_WAV = (
        Path(args.flac_to_wav_build_dir).resolve() / "flac_to_wav"
        if args.flac_to_wav_build_dir
        else DEFAULT_FLAC_TO_WAV
    )
    block_sizes = (
        [int(s.strip()) for s in args.block_sizes.split(",")]
        if args.block_sizes
        else DEFAULT_BLOCK_SIZES
    )

    print("FLAC Encoder Test Suite")
    print("=" * 40)

    if not WAV_TO_FLAC.exists():
        print(f"Error: wav_to_flac not found at {WAV_TO_FLAC}")
        print("Please build it first:")
        print("  cd host_examples/wav_to_flac")
        print("  cmake -DENABLE_SANITIZERS=ON -B build && cmake --build build")
        return 1

    if not FLAC_TO_WAV.exists():
        print(f"Error: flac_to_wav not found at {FLAC_TO_WAV}")
        print("Please build it first:")
        print("  cd host_examples/flac_to_wav")
        print("  cmake -DENABLE_SANITIZERS=ON -B build && cmake --build build")
        return 1

    HAVE_FFMPEG, _, _ = run_command("ffmpeg -version")
    HAVE_FLAC, _, _ = run_command("flac --version")

    print(f"Found wav_to_flac at {WAV_TO_FLAC}")
    print(f"Found flac_to_wav at {FLAC_TO_WAV}")
    print(f"ffmpeg available: {HAVE_FFMPEG}")
    print(f"flac CLI available: {HAVE_FLAC}")
    print(f"Block sizes: {block_sizes}")
    print()

    skip_notes = []
    if not HAVE_FFMPEG:
        skip_notes.append("ffmpeg not found -- ffmpeg round-trip checks and real-audio sources skipped")
    if not HAVE_FLAC:
        skip_notes.append("flac CLI not found -- `flac -t` checks skipped")

    # ------------------------------------------------------------------
    # Build sources
    # ------------------------------------------------------------------
    print("Generating synthetic WAV sources...")
    synthetic_sources = build_synthetic_sources(SYNTH_WAV_DIR)
    print(f"  {len(synthetic_sources)} synthetic sources generated")

    short_tail_sources = build_short_tail_sources(SYNTH_WAV_DIR)
    print(f"  {len(short_tail_sources)} final-frame edge-case sources generated")

    depth_channel_sources = []
    if args.skip_depth_channel:
        skip_notes.append("--skip-depth-channel passed -- bit-depth/channel-count sources skipped")
    else:
        depth_channel_sources = build_depth_channel_sources(DEPTH_WAV_DIR)
        print(f"  {len(depth_channel_sources)} bit-depth/channel-count sources generated")

    real_sources = []
    if args.skip_real_audio:
        skip_notes.append("--skip-real-audio passed -- real-audio sources skipped")
    else:
        print("Decoding real-audio sources from test/flac-test-files/subset...")
        real_sources, real_skip_notes = build_real_audio_sources(REAL_WAV_DIR)
        skip_notes.extend(real_skip_notes)
        print(f"  {len(real_sources)} real-audio sources ready ({len(real_skip_notes)} skipped)")

    all_matrix_sources = synthetic_sources + real_sources

    ENCODED_DIR.mkdir(parents=True, exist_ok=True)
    DECODED_DIR.mkdir(parents=True, exist_ok=True)

    # ------------------------------------------------------------------
    # Main block-size matrix
    # ------------------------------------------------------------------
    all_results = []
    # The depth/channel matrix runs at two representative block sizes rather
    # than the full sweep: what it varies is orthogonal to block size, and
    # 8-channel 24-bit sources are the slowest in the suite. 4096 is a
    # block-size table entry; 1000 is not (16-bit escape code), and leaves a
    # 50-sample final frame from the half-second sources.
    depth_channel_block_sizes = [4096, 1000]
    # The Rice partitioning pass: the highest order at block sizes that allow
    # different partition counts (16: at most 2 with an order-4 predictor;
    # 1000 = 8 * 125: at most 8; 1152: at most 64); then the final-frame edge
    # cases (odd and short frames, which cannot be split) and the depth/channel
    # matrix.
    partition_matrix = [
        (["--partition-order", "6"], bs) for bs in (16, 1000, 1152, 4096)
    ]
    if args.skip_partitions:
        skip_notes.append("--skip-partitions passed -- Rice partitioning pass skipped")
        partition_matrix = []
    total_cases = (
        len(all_matrix_sources) * len(block_sizes)
        + len(short_tail_sources)
        + len(depth_channel_sources) * len(depth_channel_block_sizes)
        + len(all_matrix_sources) * len(partition_matrix)
        + (len(short_tail_sources) + len(depth_channel_sources) if partition_matrix else 0)
    )
    case_idx = 0

    print()
    print("=" * 40)
    print("BLOCK-SIZE MATRIX")
    print("=" * 40)
    for source in all_matrix_sources:
        print(f"\n  {source.name} ({source.channels}ch)...")
        for bs in block_sizes:
            case_idx += 1
            result = run_case(source, ["--block-size", str(bs)], f"bs{bs}", block_size=bs)
            all_results.append(result)
            print(f"    [{case_idx}/{total_cases}] block-size={bs}... {result.message.split(' - ')[0]}")

    # ------------------------------------------------------------------
    # Final-frame edge cases: a final frame under 16 samples, and none at all
    # ------------------------------------------------------------------
    print()
    print("=" * 40)
    print("FINAL FRAME EDGE CASES (< 16 samples, exact block multiple)")
    print("=" * 40)
    for source in short_tail_sources:
        case_idx += 1
        result = run_case(source, ["--block-size", "4096"], "shorttail", block_size=4096)
        all_results.append(result)
        print(f"  [{case_idx}/{total_cases}] {source.name}... {result.message.split(' - ')[0]}")

    # ------------------------------------------------------------------
    # Bit depth and channel count matrix
    # ------------------------------------------------------------------
    if depth_channel_sources:
        print()
        print("=" * 40)
        print("BIT DEPTH / CHANNEL COUNT")
        print("=" * 40)
        for source in depth_channel_sources:
            print(f"\n  {source.name} ({source.channels}ch, {source.bits}-bit)...")
            for bs in depth_channel_block_sizes:
                case_idx += 1
                result = run_case(source, ["--block-size", str(bs)], f"bs{bs}", block_size=bs)
                all_results.append(result)
                print(
                    f"    [{case_idx}/{total_cases}] block-size={bs}..."
                    f" {result.message.split(' - ')[0]}"
                )

    # ------------------------------------------------------------------
    # Rice partitioning (FLACEncoderOptions::max_rice_partition_order)
    # ------------------------------------------------------------------
    if partition_matrix:
        print()
        print("=" * 40)
        print("RICE PARTITIONING")
        print("=" * 40)
        for source in all_matrix_sources:
            print(f"\n  {source.name} ({source.channels}ch)...")
            for part_args, bs in partition_matrix:
                case_idx += 1
                tag = "part" + "".join(a.strip("-") for a in part_args) + f"_bs{bs}"
                result = run_case(source, part_args + ["--block-size", str(bs)], tag, block_size=bs)
                all_results.append(result)
                print(
                    f"    [{case_idx}/{total_cases}] {' '.join(part_args)} block-size={bs}..."
                    f" {result.message.split(' - ')[0]}"
                )
        tail_args = ["--partition-order", "6", "--block-size", "4096"]
        for source in short_tail_sources:
            case_idx += 1
            result = run_case(source, tail_args, "part_shorttail", block_size=4096)
            all_results.append(result)
            print(
                f"  [{case_idx}/{total_cases}] {source.name} --partition-order 6..."
                f" {result.message.split(' - ')[0]}"
            )
        depth_args = ["--partition-order", "6", "--block-size", "4096"]
        for source in depth_channel_sources:
            case_idx += 1
            result = run_case(source, depth_args, "part_bs4096", block_size=4096)
            all_results.append(result)
            print(
                f"  [{case_idx}/{total_cases}] {source.name} ({source.channels}ch, {source.bits}-bit)"
                f" --partition-order 6... {result.message.split(' - ')[0]}"
            )

    # ------------------------------------------------------------------
    # Report + summary
    # ------------------------------------------------------------------
    print("\nGenerating report...")
    report_text, report_file, json_file = generate_report(all_results, skip_notes)

    stats = {
        "total": len(all_results),
        "passed": sum(1 for r in all_results if r.passed is True),
        "failed": sum(1 for r in all_results if r.passed is False),
    }

    print("\n" + "=" * 40)
    print("TEST COMPLETE")
    print("=" * 40)
    print(f"Total: {stats['total']} cases")
    print(f"Passed: {stats['passed']} ({stats['passed'] * 100 // stats['total'] if stats['total'] else 0}%)")
    print(f"Failed: {stats['failed']}")
    if skip_notes:
        print(f"Notes: {len(skip_notes)} (see report for details)")
    print()
    print(f"Full report saved to: {report_file}")
    print(f"JSON report saved to: {json_file}")

    if stats["failed"] > 0:
        print("\nFailed cases:")
        for r in all_results:
            if r.passed is False:
                print(f"  {r.name} [{r.args_desc}]: {r.message}")

    return 0 if stats["failed"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
