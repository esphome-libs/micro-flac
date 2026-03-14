#!/usr/bin/env python3
"""
FLAC Decoder Test Script
Tests the microFLAC decoder against ffmpeg for bit-perfect output
"""

import argparse
import sys
import subprocess
import hashlib
import json
from pathlib import Path
from datetime import datetime

# Configuration
SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_FLAC_TO_WAV = SCRIPT_DIR / "build" / "flac_to_wav"
TEST_FILES_DIR = SCRIPT_DIR / ".." / ".." / "test" / "flac-test-files"
RESULTS_DIR = SCRIPT_DIR / "test_results"
OGG_FLAC_DIR = RESULTS_DIR / "ogg_flac_files"
DEFAULT_CHUNK_SIZES = [1, 37, 256, 1024]

# Test categories
TEST_CATEGORIES = ["subset", "uncommon", "faulty"]

class TestResult:
    def __init__(self, filename, category):
        self.filename = filename
        self.category = category
        self.our_decode_status = None
        self.our_decode_error = None
        self.ffmpeg_decode_status = None
        self.ffmpeg_decode_error = None
        self.comparison_result = None
        self.test_passed = None  # Primary result: based on MD5 verification
        self.pcm_match = None    # Secondary result: ffmpeg comparison
        self.our_md5 = None
        self.ffmpeg_md5 = None
        self.header_md5 = None
        self.md5_match = None

def run_command(cmd, timeout=30):
    """Run a command and return (success, stdout, stderr)"""
    try:
        result = subprocess.run(
            cmd,
            shell=True,
            capture_output=True,
            text=True,
            timeout=timeout
        )
        return result.returncode == 0, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return False, "", "Command timed out"
    except Exception as e:
        return False, "", str(e)

def extract_pcm_from_wav(wav_file):
    """Extract raw PCM data from WAV file (skip headers)"""
    try:
        with open(wav_file, 'rb') as f:
            # Read first 12 bytes to verify it's a WAV file
            riff = f.read(4)
            if riff != b'RIFF':
                return None

            f.read(4)  # File size
            wave = f.read(4)
            if wave != b'WAVE':
                return None

            # Find the data chunk
            while True:
                chunk_id = f.read(4)
                if not chunk_id:
                    return None

                chunk_size = int.from_bytes(f.read(4), 'little')

                if chunk_id == b'data':
                    # Found data chunk, read PCM data
                    return f.read(chunk_size)
                else:
                    # Skip this chunk
                    f.seek(chunk_size, 1)
    except Exception as e:
        print(f"Error reading WAV file {wav_file}: {e}")
        return None

def compute_md5(data):
    """Compute MD5 hash of data"""
    if data is None:
        return None
    return hashlib.md5(data).hexdigest()

def get_flac_bit_depth(flac_file):
    """Get the bit depth of a FLAC file using ffprobe"""
    cmd = f'ffprobe -v error -select_streams a:0 -show_entries stream=bits_per_raw_sample -of default=noprint_wrappers=1:nokey=1 "{flac_file}"'
    success, stdout, stderr = run_command(cmd)
    if success and stdout.strip():
        try:
            return int(stdout.strip())
        except ValueError:
            pass
    return None

def get_ffmpeg_codec_for_bit_depth(bit_depth):
    """Get the appropriate ffmpeg codec for a given bit depth"""
    if bit_depth is None:
        return ""  # Let ffmpeg decide
    elif bit_depth <= 8:
        return "-c:a pcm_u8"
    elif bit_depth <= 16:
        return "-c:a pcm_s16le"
    elif bit_depth <= 24:
        return "-c:a pcm_s24le"
    elif bit_depth <= 32:
        return "-c:a pcm_s32le"
    else:
        return ""  # Let ffmpeg decide

def extract_md5_from_output(stdout):
    """Extract MD5 signature from decoder output"""
    for line in stdout.splitlines():
        if "MD5 signature:" in line:
            # Extract the hex string after "MD5 signature:"
            md5_str = line.split("MD5 signature:")[1].strip()
            return md5_str
    return None

def extract_md5_verification_from_output(stdout):
    """Extract MD5 verification result from decoder output
    Returns (expected_md5, computed_md5, passed) or (None, None, None) if not found
    """
    expected = None
    computed = None
    passed = None

    lines = stdout.splitlines()
    for i, line in enumerate(lines):
        if "Expected MD5:" in line:
            expected = line.split("Expected MD5:")[1].strip()
        elif "Computed MD5:" in line:
            computed = line.split("Computed MD5:")[1].strip()
        elif "Result: PASS" in line:
            passed = True
        elif "Result: FAIL" in line:
            passed = False
        elif "Status: SKIPPED" in line:
            # MD5 not available in file
            return None, None, None

    return expected, computed, passed

def test_streaming_file(flac_file, chunk_size):
    """Test a single FLAC file using streaming decode with given chunk size"""
    base_name = Path(flac_file).stem
    streaming_output = RESULTS_DIR / "streaming" / f"{base_name}_chunk{chunk_size}.wav"
    streaming_output.parent.mkdir(parents=True, exist_ok=True)

    cmd = f'"{FLAC_TO_WAV}" --streaming {chunk_size} "{flac_file}" "{streaming_output}"'
    success, stdout, stderr = run_command(cmd, timeout=120)

    if not success:
        return False, f"Decode failed: {stderr.strip() if stderr else stdout.strip()}"

    # Check MD5 verification from output
    expected_md5, computed_md5, md5_passed = extract_md5_verification_from_output(stdout)
    if md5_passed is True:
        return True, "PASS - MD5 verified"
    elif md5_passed is False:
        return False, f"FAIL - MD5 mismatch (expected: {expected_md5}, computed: {computed_md5})"
    else:
        # No MD5 in file - compare with normal decode output
        normal_output = RESULTS_DIR / "subset" / "our_decoder" / f"{base_name}.wav"
        if normal_output.exists():
            normal_pcm = extract_pcm_from_wav(normal_output)
            streaming_pcm = extract_pcm_from_wav(streaming_output)
            if normal_pcm is not None and streaming_pcm is not None:
                if normal_pcm == streaming_pcm:
                    return True, "PASS - Matches normal decode"
                else:
                    return False, f"FAIL - PCM mismatch with normal decode ({len(normal_pcm)} vs {len(streaming_pcm)} bytes)"
        return None, "UNKNOWN - No MD5 and no normal decode reference"


def test_single_file(flac_file, category):
    """Test a single FLAC file"""
    result = TestResult(Path(flac_file).name, category)

    # Get bit depth to use appropriate ffmpeg codec
    bit_depth = get_flac_bit_depth(flac_file)

    # Create output paths
    base_name = Path(flac_file).stem
    our_output = RESULTS_DIR / category / "our_decoder" / f"{base_name}.wav"
    ffmpeg_output = RESULTS_DIR / category / "ffmpeg" / f"{base_name}.wav"

    # Ensure output directories exist
    our_output.parent.mkdir(parents=True, exist_ok=True)
    ffmpeg_output.parent.mkdir(parents=True, exist_ok=True)

    # Test our decoder
    cmd = f'"{FLAC_TO_WAV}" "{flac_file}" "{our_output}"'
    success, stdout, stderr = run_command(cmd)
    result.our_decode_status = "success" if success else "failed"
    if not success:
        result.our_decode_error = stderr.strip() if stderr else stdout.strip()
    else:
        # Extract MD5 verification from decoder output
        expected_md5, computed_md5, md5_passed = extract_md5_verification_from_output(stdout)
        result.header_md5 = expected_md5
        result.our_md5 = computed_md5
        if md5_passed is not None:
            result.md5_match = md5_passed

    # Test ffmpeg decoder with appropriate codec for bit depth
    codec_arg = get_ffmpeg_codec_for_bit_depth(bit_depth)
    cmd = f'ffmpeg -i "{flac_file}" {codec_arg} -f wav -y "{ffmpeg_output}" 2>&1'
    success, stdout, stderr = run_command(cmd)
    result.ffmpeg_decode_status = "success" if success else "failed"
    if not success:
        result.ffmpeg_decode_error = (stderr + stdout).strip()

    # Determine test result based on MD5 verification (primary)
    if result.our_decode_status == "success":
        if result.md5_match is True:
            # MD5 verification passed - this is definitive proof of correct decode
            result.test_passed = True
            result.comparison_result = "PASS - MD5 verified"
        elif result.md5_match is False:
            # MD5 verification failed - decode is incorrect
            result.test_passed = False
            result.comparison_result = "FAIL - MD5 mismatch"
        elif result.md5_match is None:
            # No MD5 in file - fall back to ffmpeg comparison
            if result.ffmpeg_decode_status == "success":
                our_pcm = extract_pcm_from_wav(our_output)
                ffmpeg_pcm = extract_pcm_from_wav(ffmpeg_output)

                if our_pcm is not None and ffmpeg_pcm is not None:
                    result.ffmpeg_md5 = compute_md5(ffmpeg_pcm)
                    if our_pcm == ffmpeg_pcm:
                        result.pcm_match = True
                        result.test_passed = True
                        result.comparison_result = "PASS - Matches ffmpeg (no MD5 in file)"
                    else:
                        result.pcm_match = False
                        result.test_passed = False
                        result.comparison_result = f"FAIL - PCM mismatch with ffmpeg (our: {len(our_pcm)} bytes, ffmpeg: {len(ffmpeg_pcm)} bytes)"
                else:
                    result.test_passed = None
                    result.comparison_result = "ERROR - Could not extract PCM data"
            else:
                # Our decoder succeeded but no MD5 and ffmpeg failed
                result.test_passed = None
                result.comparison_result = "UNKNOWN - No MD5 available and ffmpeg failed"

        # Secondary check: compare with ffmpeg if both succeeded (for additional validation)
        if result.ffmpeg_decode_status == "success" and result.md5_match is not None:
            our_pcm = extract_pcm_from_wav(our_output)
            ffmpeg_pcm = extract_pcm_from_wav(ffmpeg_output)

            if our_pcm is not None and ffmpeg_pcm is not None:
                result.ffmpeg_md5 = compute_md5(ffmpeg_pcm)
                result.pcm_match = (our_pcm == ffmpeg_pcm)

                # Add note if ffmpeg disagrees with MD5 result
                if result.test_passed and not result.pcm_match:
                    result.comparison_result += " (WARNING: ffmpeg output differs)"
                elif not result.test_passed and result.pcm_match:
                    result.comparison_result += " (NOTE: ffmpeg output matches despite MD5 fail)"
                elif result.test_passed and result.pcm_match:
                    result.comparison_result += " + matches ffmpeg"

    elif result.our_decode_status == "failed":
        result.test_passed = False
        if result.ffmpeg_decode_status == "failed":
            result.comparison_result = "EXPECTED - Both decoders failed (likely invalid file)"
            result.test_passed = None  # Expected failure, not a real failure
        else:
            result.comparison_result = "FAIL - Decoder failed but ffmpeg succeeded"
    else:
        result.test_passed = None
        result.comparison_result = "ERROR - Decode status unknown"

    return result

def test_category(category):
    """Test all files in a category"""
    category_dir = TEST_FILES_DIR / category
    results = []

    # Find all FLAC files in the category
    flac_files = sorted(category_dir.glob("*.flac"))

    print(f"\nTesting {category} files ({len(flac_files)} files)...")

    for i, flac_file in enumerate(flac_files, 1):
        print(f"  [{i}/{len(flac_files)}] Testing {flac_file.name}...", end="")
        result = test_single_file(str(flac_file), category)
        results.append(result)

        # Print immediate result based on test_passed
        if result.test_passed is True:
            print(" PASS")
        elif result.test_passed is False:
            print(" FAIL")
        else:
            print(f" - {result.comparison_result[:40]}")

    return results

def test_streaming(chunk_sizes=None):
    """Test streaming decode with various chunk sizes on subset files"""
    if chunk_sizes is None:
        chunk_sizes = DEFAULT_CHUNK_SIZES

    category_dir = TEST_FILES_DIR / "subset"
    flac_files = sorted(category_dir.glob("*.flac"))

    streaming_results = []
    all_passed = True

    for chunk_size in chunk_sizes:
        print(f"\n  Streaming test with chunk_size={chunk_size} ({len(flac_files)} files)...")
        chunk_passed = 0
        chunk_failed = 0

        for i, flac_file in enumerate(flac_files, 1):
            print(f"    [{i}/{len(flac_files)}] {flac_file.name} (chunk={chunk_size})...", end="")
            passed, msg = test_streaming_file(str(flac_file), chunk_size)

            if passed is True:
                print(" PASS")
                chunk_passed += 1
            elif passed is False:
                print(f" FAIL - {msg}")
                chunk_failed += 1
                all_passed = False
            else:
                print(f" ? - {msg}")

            streaming_results.append({
                "file": flac_file.name,
                "chunk_size": chunk_size,
                "passed": passed,
                "message": msg
            })

        print(f"  chunk_size={chunk_size}: {chunk_passed} passed, {chunk_failed} failed")

    return streaming_results, all_passed


def convert_flac_to_ogg(flac_file, ogg_output):
    """Convert a FLAC file to Ogg FLAC, preserving MD5 signature.
    Tries flac CLI first (re-encodes, preserves MD5), falls back to
    ffmpeg stream copy for files flac CLI can't handle."""
    cmd = f'flac --ogg -f -o "{ogg_output}" "{flac_file}" 2>&1'
    success, stdout, stderr = run_command(cmd, timeout=30)
    if success:
        return True
    cmd = f'ffmpeg -i "{flac_file}" -c:a copy -y "{ogg_output}" 2>&1'
    success, stdout, stderr = run_command(cmd, timeout=30)
    return success


def generate_ogg_flac_files(category="subset"):
    """Generate Ogg FLAC versions of test files for a given category"""
    category_dir = TEST_FILES_DIR / category
    ogg_dir = OGG_FLAC_DIR / category
    ogg_dir.mkdir(parents=True, exist_ok=True)

    flac_files = sorted(category_dir.glob("*.flac"))
    ogg_files = []
    failed = []

    for flac_file in flac_files:
        ogg_file = ogg_dir / (flac_file.stem + ".oga")
        if not ogg_file.exists():
            if convert_flac_to_ogg(str(flac_file), str(ogg_file)):
                ogg_files.append((str(flac_file), str(ogg_file)))
            else:
                failed.append(flac_file.name)
        else:
            ogg_files.append((str(flac_file), str(ogg_file)))

    return ogg_files, failed


def test_ogg_flac_file(ogg_file, native_wav_file):
    """Test a single Ogg FLAC file by decoding and comparing with native FLAC output"""
    base_name = Path(ogg_file).stem
    ogg_output = RESULTS_DIR / "ogg_flac" / f"{base_name}.wav"
    ogg_output.parent.mkdir(parents=True, exist_ok=True)

    cmd = f'"{FLAC_TO_WAV}" "{ogg_file}" "{ogg_output}"'
    success, stdout, stderr = run_command(cmd, timeout=60)

    if not success:
        return False, f"Decode failed: {stderr.strip() if stderr else stdout.strip()}"

    # Check MD5 verification from output
    expected_md5, computed_md5, md5_passed = extract_md5_verification_from_output(stdout)
    if md5_passed is True:
        # Also compare PCM with native FLAC output for extra validation
        if Path(native_wav_file).exists():
            ogg_pcm = extract_pcm_from_wav(ogg_output)
            native_pcm = extract_pcm_from_wav(native_wav_file)
            if ogg_pcm is not None and native_pcm is not None and ogg_pcm != native_pcm:
                return False, "FAIL - MD5 passed but PCM differs from native FLAC"
        return True, "PASS - MD5 verified"
    elif md5_passed is False:
        return False, f"FAIL - MD5 mismatch (expected: {expected_md5}, computed: {computed_md5})"
    else:
        # No MD5 - compare with ffmpeg's decode of the Ogg file itself
        # (Can't compare with native FLAC since ffmpeg may change bit depth during Ogg muxing,
        # e.g. 8-bit FLAC gets upconverted to 16-bit in Ogg FLAC)
        ffmpeg_ogg_output = RESULTS_DIR / "ogg_flac" / f"{base_name}_ffmpeg.wav"
        bit_depth = get_flac_bit_depth(ogg_file)
        codec_arg = get_ffmpeg_codec_for_bit_depth(bit_depth)
        cmd = f'ffmpeg -i "{ogg_file}" {codec_arg} -f wav -y "{ffmpeg_ogg_output}" 2>&1'
        ff_success, _, _ = run_command(cmd)
        if ff_success:
            ogg_pcm = extract_pcm_from_wav(ogg_output)
            ffmpeg_pcm = extract_pcm_from_wav(ffmpeg_ogg_output)
            if ogg_pcm is not None and ffmpeg_pcm is not None:
                if ogg_pcm == ffmpeg_pcm:
                    return True, "PASS - Matches ffmpeg decode of Ogg (no MD5 in file)"
                else:
                    return False, f"FAIL - PCM mismatch with ffmpeg Ogg decode ({len(ogg_pcm)} vs {len(ffmpeg_pcm)} bytes)"
        return None, "UNKNOWN - No MD5 and ffmpeg Ogg decode failed"


def test_ogg_flac_streaming(ogg_file, chunk_size):
    """Test a single Ogg FLAC file with streaming decode"""
    base_name = Path(ogg_file).stem
    streaming_output = RESULTS_DIR / "ogg_streaming" / f"{base_name}_chunk{chunk_size}.wav"
    streaming_output.parent.mkdir(parents=True, exist_ok=True)

    cmd = f'"{FLAC_TO_WAV}" --streaming {chunk_size} "{ogg_file}" "{streaming_output}"'
    success, stdout, stderr = run_command(cmd, timeout=120)

    if not success:
        return False, f"Decode failed: {stderr.strip() if stderr else stdout.strip()}"

    # Check MD5 verification from output
    expected_md5, computed_md5, md5_passed = extract_md5_verification_from_output(stdout)
    if md5_passed is True:
        return True, "PASS - MD5 verified"
    elif md5_passed is False:
        return False, f"FAIL - MD5 mismatch (expected: {expected_md5}, computed: {computed_md5})"
    else:
        # No MD5 - compare with normal ogg decode output
        normal_output = RESULTS_DIR / "ogg_flac" / f"{base_name}.wav"
        if normal_output.exists():
            normal_pcm = extract_pcm_from_wav(normal_output)
            streaming_pcm = extract_pcm_from_wav(streaming_output)
            if normal_pcm is not None and streaming_pcm is not None:
                if normal_pcm == streaming_pcm:
                    return True, "PASS - Matches normal Ogg decode"
                else:
                    return False, f"FAIL - PCM mismatch ({len(normal_pcm)} vs {len(streaming_pcm)} bytes)"
        return None, "UNKNOWN - No MD5 and no normal decode reference"


def test_32bit_file(flac_file):
    """Test a single FLAC file using --output-32bit and verify correctness"""
    base_name = Path(flac_file).stem
    output_32bit = RESULTS_DIR / "32bit" / f"{base_name}_32bit.wav"
    output_32bit.parent.mkdir(parents=True, exist_ok=True)

    cmd = f'"{FLAC_TO_WAV}" --output-32bit "{flac_file}" "{output_32bit}"'
    success, stdout, stderr = run_command(cmd, timeout=120)

    if not success:
        # Check if normal decode also fails (expected failure for faulty files)
        normal_check = RESULTS_DIR / "32bit" / f"{base_name}_normal_check.wav"
        cmd_check = f'"{FLAC_TO_WAV}" "{flac_file}" "{normal_check}"'
        norm_success, _, _ = run_command(cmd_check, timeout=30)
        if not norm_success:
            return None, "EXPECTED - Both normal and 32-bit decode failed"
        return False, f"Decode failed: {stderr.strip() if stderr else stdout.strip()}"

    # Check MD5 verification from output
    expected_md5, computed_md5, md5_passed = extract_md5_verification_from_output(stdout)
    if md5_passed is False:
        return False, f"FAIL - MD5 mismatch (expected: {expected_md5}, computed: {computed_md5})"

    # Cross-compare: decode normally, then verify 32-bit output matches native left-shifted
    normal_output = RESULTS_DIR / "32bit" / f"{base_name}_normal.wav"
    cmd_normal = f'"{FLAC_TO_WAV}" "{flac_file}" "{normal_output}"'
    norm_success, norm_stdout, _ = run_command(cmd_normal, timeout=120)

    if not norm_success:
        if md5_passed is True:
            return True, "PASS - MD5 verified (normal decode unavailable for cross-check)"
        return None, "UNKNOWN - Normal decode failed and no MD5"

    # Get bit depth from the normal decode output
    bit_depth = get_flac_bit_depth(flac_file)
    if bit_depth is None:
        if md5_passed is True:
            return True, "PASS - MD5 verified (bit depth unknown for cross-check)"
        return None, "UNKNOWN - Could not determine bit depth"

    # Read 32-bit WAV samples (always 32-bit signed)
    pcm_32 = extract_pcm_from_wav(output_32bit)
    pcm_native = extract_pcm_from_wav(normal_output)

    if pcm_32 is None or pcm_native is None:
        if md5_passed is True:
            return True, "PASS - MD5 verified (could not read WAV for cross-check)"
        return None, "UNKNOWN - Could not extract PCM data"

    native_bytes = (bit_depth + 7) // 8
    num_samples = len(pcm_native) // native_bytes

    if len(pcm_32) != num_samples * 4:
        return False, f"FAIL - Sample count mismatch: 32-bit has {len(pcm_32)//4} samples, native has {num_samples}"

    # Compare: left-shift native samples to 32-bit and verify they match
    shift = 32 - bit_depth
    mismatch_count = 0
    first_mismatch = None

    for i in range(num_samples):
        # Read native sample
        if bit_depth == 8:
            # 8-bit WAV is unsigned (0-255), convert to signed for comparison
            native_val = pcm_native[i] - 128
        else:
            native_val = int.from_bytes(
                pcm_native[i * native_bytes:(i + 1) * native_bytes], 'little',
                signed=True)

        # For non-byte-aligned, the native output is LSB-padded in the container
        # The decoder pads non-byte-aligned samples by left-shifting within their container
        # So a 12-bit sample in a 16-bit container has the sample in the upper 12 bits
        # We need to account for this padding
        container_bits = native_bytes * 8
        if bit_depth != container_bits:
            # Native output is already left-shifted within its container by (container_bits - bit_depth)
            # To get true 32-bit left-justified, shift by (32 - container_bits)
            expected_32 = native_val << (32 - container_bits)
        else:
            expected_32 = native_val << shift

        # Handle sign extension to int32
        if expected_32 >= (1 << 31):
            expected_32 -= (1 << 32)
        if expected_32 < -(1 << 31):
            expected_32 += (1 << 32)

        # Read 32-bit sample
        actual_32 = int.from_bytes(pcm_32[i * 4:(i + 1) * 4], 'little', signed=True)

        if actual_32 != expected_32:
            mismatch_count += 1
            if first_mismatch is None:
                first_mismatch = (i, expected_32, actual_32)

    if mismatch_count > 0:
        idx, expected, actual = first_mismatch
        return False, f"FAIL - {mismatch_count} sample mismatches (first at [{idx}]: expected {expected}, got {actual})"

    if md5_passed is True:
        return True, "PASS - MD5 verified + cross-check passed"
    elif md5_passed is None:
        return True, "PASS - Cross-check passed (no MD5 in file)"
    else:
        return True, "PASS - Cross-check passed"


def test_32bit_output(category="subset"):
    """Test 32-bit output mode for all files in a category"""
    category_dir = TEST_FILES_DIR / category
    flac_files = sorted(category_dir.glob("*.flac"))

    print(f"\n  32-bit output test ({len(flac_files)} files)...")
    passed_count = 0
    failed_count = 0

    for i, flac_file in enumerate(flac_files, 1):
        print(f"    [{i}/{len(flac_files)}] {flac_file.name}...", end="")
        passed, msg = test_32bit_file(str(flac_file))

        if passed is True:
            print(" PASS")
            passed_count += 1
        elif passed is False:
            print(f" FAIL - {msg}")
            failed_count += 1
        else:
            print(f" ? - {msg}")

    print(f"  32-bit output: {passed_count} passed, {failed_count} failed")
    return failed_count == 0


def test_ogg_flac(category="subset", run_normal=True, run_streaming=True, chunk_sizes=None):
    """Run Ogg FLAC tests: normal decode and/or streaming with various chunk sizes"""
    if chunk_sizes is None:
        chunk_sizes = DEFAULT_CHUNK_SIZES

    print("\n" + "=" * 40)
    print("OGG FLAC TESTS")
    print("=" * 40)

    # Generate Ogg FLAC files
    print("\nGenerating Ogg FLAC test files...")
    ogg_files, gen_failed = generate_ogg_flac_files(category)
    print(f"  Generated {len(ogg_files)} files, {len(gen_failed)} failed")
    for name in gen_failed:
        print(f"    SKIP - Could not convert: {name}")

    if not ogg_files:
        print("  No Ogg FLAC files to test!")
        return True

    all_passed = True

    if run_normal:
        # Normal decode test
        print(f"\n  Normal Ogg FLAC decode ({len(ogg_files)} files)...")
        normal_passed = 0
        normal_failed = 0

        for i, (native_flac, ogg_file) in enumerate(ogg_files, 1):
            base_name = Path(ogg_file).name
            native_wav = RESULTS_DIR / category / "our_decoder" / (Path(native_flac).stem + ".wav")
            print(f"    [{i}/{len(ogg_files)}] {base_name}...", end="")
            passed, msg = test_ogg_flac_file(ogg_file, native_wav)

            if passed is True:
                print(" PASS")
                normal_passed += 1
            elif passed is False:
                print(f" FAIL - {msg}")
                normal_failed += 1
                all_passed = False
            else:
                print(f" ? - {msg}")

        print(f"  Normal Ogg decode: {normal_passed} passed, {normal_failed} failed")

    if not run_streaming:
        return all_passed

    # Streaming tests
    for chunk_size in chunk_sizes:
        print(f"\n  Ogg FLAC streaming chunk_size={chunk_size} ({len(ogg_files)} files)...")
        chunk_passed = 0
        chunk_failed = 0

        for i, (native_flac, ogg_file) in enumerate(ogg_files, 1):
            base_name = Path(ogg_file).name
            print(f"    [{i}/{len(ogg_files)}] {base_name} (chunk={chunk_size})...", end="")
            passed, msg = test_ogg_flac_streaming(ogg_file, chunk_size)

            if passed is True:
                print(" PASS")
                chunk_passed += 1
            elif passed is False:
                print(f" FAIL - {msg}")
                chunk_failed += 1
                all_passed = False
            else:
                print(f" ? - {msg}")

        print(f"  chunk_size={chunk_size}: {chunk_passed} passed, {chunk_failed} failed")

    return all_passed


def generate_report(all_results):
    """Generate test report"""
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    # Calculate statistics
    stats = {
        "total": len(all_results),
        "passed": sum(1 for r in all_results if r.test_passed is True),
        "failed": sum(1 for r in all_results if r.test_passed is False),
        "errors": sum(1 for r in all_results if r.test_passed is None),
    }

    # Generate text report
    report_lines = [
        "=" * 80,
        "FLAC Decoder Test Report",
        f"Generated: {timestamp}",
        "=" * 80,
        "",
        "SUMMARY",
        "-" * 40,
        f"Total files tested: {stats['total']}",
        f"Passed (bit-perfect): {stats['passed']} ({stats['passed']*100//stats['total'] if stats['total'] else 0}%)",
        f"Failed: {stats['failed']}",
        f"Errors/Expected failures: {stats['errors']}",
        "",
    ]

    # Detailed results by category
    for category in TEST_CATEGORIES:
        category_results = [r for r in all_results if r.category == category]
        if not category_results:
            continue

        report_lines.extend([
            "",
            f"{category.upper()} FILES ({len(category_results)} files)",
            "-" * 40,
        ])

        for result in category_results:
            status = "PASS" if result.test_passed is True else "FAIL" if result.test_passed is False else "?"
            report_lines.append(f"{status} {result.filename}: {result.comparison_result}")

            # Show MD5 details if there was a mismatch
            if result.md5_match is False and result.header_md5:
                report_lines.append(f"    Expected MD5: {result.header_md5}")
                report_lines.append(f"    Computed MD5: {result.our_md5}")

            # Show ffmpeg comparison details if relevant
            if result.pcm_match is not None:
                ffmpeg_status = "matches" if result.pcm_match else "differs"
                report_lines.append(f"    ffmpeg comparison: {ffmpeg_status}")

            if result.our_decode_error:
                report_lines.append(f"    Our decoder error: {result.our_decode_error}")
            if result.ffmpeg_decode_error and "ffmpeg failed" in result.comparison_result:
                report_lines.append(f"    ffmpeg error: {result.ffmpeg_decode_error[:100]}")

    # Ensure results directory exists
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)

    # Write text report
    report_text = "\n".join(report_lines)
    report_file = RESULTS_DIR / "test_report.txt"
    with open(report_file, "w") as f:
        f.write(report_text)

    # Also save JSON report for programmatic access
    json_report = {
        "timestamp": timestamp,
        "summary": stats,
        "results": [
            {
                "file": r.filename,
                "category": r.category,
                "our_status": r.our_decode_status,
                "ffmpeg_status": r.ffmpeg_decode_status,
                "test_passed": r.test_passed,
                "pcm_match": r.pcm_match,
                "md5_match": r.md5_match,
                "result": r.comparison_result,
                "header_md5": r.header_md5,
                "our_md5": r.our_md5,
                "ffmpeg_md5": r.ffmpeg_md5,
            }
            for r in all_results
        ]
    }

    json_file = RESULTS_DIR / "test_report.json"
    with open(json_file, "w") as f:
        json.dump(json_report, f, indent=2)

    return report_text, report_file, json_file

def main():
    """Main test runner"""
    parser = argparse.ArgumentParser(description="FLAC Decoder Test Suite")
    parser.add_argument(
        "--format",
        choices=["all", "flac", "ogg"],
        default="all",
        help="Which formats to test: flac (native only), ogg (Ogg FLAC only), all (default)",
    )
    parser.add_argument(
        "--mode",
        choices=["all", "normal", "streaming", "32bit"],
        default="normal",
        help="Which test modes to run: normal (full-frame decode, default), streaming (chunked), 32bit (32-bit output), all",
    )
    parser.add_argument(
        "--chunk-sizes",
        type=str,
        default=None,
        help="Comma-separated chunk sizes for streaming tests (default: 1,37,256,1024)",
    )
    parser.add_argument(
        "--build-dir",
        type=str,
        default=None,
        help="Path to build directory containing flac_to_wav binary",
    )
    args = parser.parse_args()

    global FLAC_TO_WAV
    if args.build_dir:
        FLAC_TO_WAV = Path(args.build_dir).resolve() / "flac_to_wav"
    else:
        FLAC_TO_WAV = DEFAULT_FLAC_TO_WAV

    run_flac = args.format in ("all", "flac")
    run_ogg = args.format in ("all", "ogg")
    run_normal = args.mode in ("all", "normal")
    run_streaming = args.mode in ("all", "streaming")
    run_32bit = args.mode in ("all", "32bit")

    if args.chunk_sizes:
        chunk_sizes = [int(s.strip()) for s in args.chunk_sizes.split(",")]
    else:
        chunk_sizes = DEFAULT_CHUNK_SIZES

    print("FLAC Decoder Test Suite")
    print("=" * 40)

    # Check prerequisites
    if not FLAC_TO_WAV.exists():
        print(f"Error: flac_to_wav not found at {FLAC_TO_WAV}")
        print("Please build it first:")
        print("  cmake -B build")
        print("  cmake --build build")
        return 1

    if not TEST_FILES_DIR.exists():
        print(f"Error: Test files not found at {TEST_FILES_DIR}")
        print("Please ensure the flac-test-files submodule is initialized:")
        print("  git submodule update --init")
        return 1

    # Check ffmpeg
    success, _, _ = run_command("ffmpeg -version")
    if not success:
        print("Error: ffmpeg not found. Please install ffmpeg.")
        return 1

    print(f"Found flac_to_wav at {FLAC_TO_WAV}")
    print(f"Found test files at {TEST_FILES_DIR}")
    print(f"ffmpeg is available")
    print(f"Format filter: {args.format}")
    print(f"Mode filter: {args.mode}")
    if run_streaming:
        print(f"Chunk sizes: {chunk_sizes}")
    print()

    any_failed = False

    if run_flac:
        if run_normal:
            # Run tests for each category
            all_results = []
            for category in TEST_CATEGORIES:
                results = test_category(category)
                all_results.extend(results)

            # Generate report
            print("\nGenerating report...")
            report_text, report_file, json_file = generate_report(all_results)

            # Print summary
            print("\n" + "=" * 40)
            print("TEST COMPLETE")
            print("=" * 40)

            stats = {
                "total": len(all_results),
                "passed": sum(1 for r in all_results if r.test_passed is True),
                "failed": sum(1 for r in all_results if r.test_passed is False),
                "errors": sum(1 for r in all_results if r.test_passed is None),
            }

            print(f"Total: {stats['total']} files")
            print(f"Passed: {stats['passed']} ({stats['passed']*100//stats['total'] if stats['total'] else 0}%)")
            print(f"Failed: {stats['failed']}")
            print(f"Errors/Expected: {stats['errors']}")
            print()
            print(f"Full report saved to: {report_file}")
            print(f"JSON report saved to: {json_file}")

            if stats['failed'] > 0:
                any_failed = True

        if run_streaming:
            # Run streaming tests
            print("\n" + "=" * 40)
            print("STREAMING TESTS")
            print("=" * 40)
            streaming_results, streaming_all_passed = test_streaming(chunk_sizes)

            if not streaming_all_passed:
                any_failed = True
                print("\nStreaming tests: SOME FAILURES")
            else:
                print("\nStreaming tests: ALL PASSED")

    if run_flac and run_32bit:
        print("\n" + "=" * 40)
        print("32-BIT OUTPUT TESTS")
        print("=" * 40)
        for category in TEST_CATEGORIES:
            if not test_32bit_output(category):
                any_failed = True
        if any_failed:
            print("\n32-bit output tests: SOME FAILURES")
        else:
            print("\n32-bit output tests: ALL PASSED")

    if run_ogg:
        # Run Ogg FLAC tests (respects mode filter)
        ogg_all_passed = test_ogg_flac(run_normal=run_normal, run_streaming=run_streaming, chunk_sizes=chunk_sizes)

        if not ogg_all_passed:
            any_failed = True
            print("\nOgg FLAC tests: SOME FAILURES")
        else:
            print("\nOgg FLAC tests: ALL PASSED")

    # Return non-zero if any tests failed
    return 0 if not any_failed else 1

if __name__ == "__main__":
    sys.exit(main())
