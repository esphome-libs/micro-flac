#!/usr/bin/env python3
"""Parse LPC test vector binary dump and generate a C header file.

Includes synthetic vector generation for orders not found in real FLAC data
(particularly orders > 12 which exercise the generic assembly loop).

Usage:
    python3 generate_test_vectors.py -i lpc_vectors.bin -o src/test_lpc_vectors.h
    python3 generate_test_vectors.py -i lpc_vectors.bin -o src/test_lpc_vectors.h --max-per-group 4
    python3 generate_test_vectors.py -i lpc_vectors.bin -o src/test_lpc_vectors.h --no-synthetic
"""

import argparse
import random
import struct
import sys
from collections import defaultdict

MAGIC = 0x4C504356  # "LPCV"

# INT32 limits for clamping
INT32_MIN = -(1 << 31)
INT32_MAX = (1 << 31) - 1


def parse_vectors(filepath):
    """Parse all vectors from a binary dump file."""
    vectors = []
    with open(filepath, "rb") as f:
        data = f.read()

    offset = 0
    while offset < len(data):
        if offset + 4 > len(data):
            break
        (magic,) = struct.unpack_from("<I", data, offset)
        if magic != MAGIC:
            print(f"Warning: bad magic at offset {offset}, skipping byte", file=sys.stderr)
            offset += 1
            continue

        offset += 4

        (is_64bit,) = struct.unpack_from("<B", data, offset)
        offset += 1
        order, shift, bits_per_sample, num_samples = struct.unpack_from("<IiII", data, offset)
        offset += 16

        coefs = list(struct.unpack_from(f"<{order}i", data, offset))
        offset += 4 * order

        input_buf = list(struct.unpack_from(f"<{num_samples}i", data, offset))
        offset += 4 * num_samples

        expected = list(struct.unpack_from(f"<{num_samples}i", data, offset))
        offset += 4 * num_samples

        vectors.append({
            "is_64bit": is_64bit,
            "order": order,
            "shift": shift,
            "bits_per_sample": bits_per_sample,
            "num_samples": num_samples,
            "coefs": coefs,
            "input": input_buf,
            "expected": expected,
            "synthetic": False,
        })

    return vectors


def clamp_int32(x):
    """Clamp to int32 range (mimics C int32_t overflow behavior for addition)."""
    # Use masking to simulate two's complement wrapping
    x = x & 0xFFFFFFFF
    if x >= 0x80000000:
        x -= 0x100000000
    return x


def lpc_restore_32bit(buffer, order, coefs, shift):
    """Python reference implementation of 32-bit LPC restoration.

    Must simulate int32_t wrapping on both multiply and accumulate to match the
    assembly behavior. For real FLAC data, can_use_lpc_32bit() guarantees no
    overflow so wrapping never triggers. But for synthetic test data, restored
    samples can grow beyond the assumed bit depth, causing overflow in later
    iterations. The assembly wraps via mull/add, so Python must do the same.
    """
    buf = list(buffer)
    for i in range(len(buf) - order):
        acc = 0
        for j in range(order):
            acc = clamp_int32(acc + clamp_int32(buf[i + j] * coefs[j]))
        buf[i + order] = clamp_int32(buf[i + order] + (acc >> shift))
    return buf


def lpc_restore_64bit(buffer, order, coefs, shift):
    """Python reference implementation of 64-bit LPC restoration.

    Uses arbitrary-precision Python ints for the accumulator (matching C++ int64_t).
    Python's >> is arithmetic right shift on negative numbers, matching C++ behavior.
    Final result is clamped to int32_t.
    """
    buf = list(buffer)
    for i in range(len(buf) - order):
        acc = 0
        for j in range(order):
            acc += buf[i + j] * coefs[j]
        buf[i + order] = clamp_int32(buf[i + order] + (acc >> shift))
    return buf


def can_use_32bit(bits_per_sample, coefs, order, shift):
    """Mirrors the C++ can_use_lpc_32bit() overflow analysis."""
    max_abs_sample = 1 << (bits_per_sample - 1)
    abs_sum = sum(abs(c) for c in coefs)
    max_pred_before_shift = max_abs_sample * abs_sum

    if max_pred_before_shift > INT32_MAX:
        return False

    max_pred_after_shift = -(-max_pred_before_shift >> shift) if max_pred_before_shift > 0 else 0
    return (max_abs_sample + max_pred_after_shift) <= INT32_MAX


def generate_synthetic_vector(order, is_64bit, rng, num_samples=256):
    """Generate a synthetic test vector with FLAC-realistic constraints."""
    if is_64bit:
        # 64-bit path: use 24-bit samples with larger coefficients
        bits_per_sample = 24
        max_sample = (1 << 23) - 1
        # FLAC qlp_coeff_precision is max 15 bits
        max_coef = (1 << 14) - 1
        shift = rng.randint(10, 15)
    else:
        # 32-bit path: use 16-bit samples with smaller coefficients
        bits_per_sample = 16
        max_sample = (1 << 15) - 1
        max_coef = (1 << 10) - 1
        shift = rng.randint(6, 12)

    # Generate coefficients (FLAC-realistic: signed, bounded by precision)
    coefs = [rng.randint(-max_coef, max_coef) for _ in range(order)]

    # Generate warm-up samples (bounded by bits_per_sample)
    warmup = [rng.randint(-max_sample, max_sample) for _ in range(order)]

    # Generate residuals (typically much smaller than samples in real FLAC)
    max_residual = max_sample // 4
    residuals = [rng.randint(-max_residual, max_residual)
                 for _ in range(num_samples - order)]

    input_buffer = warmup + residuals

    # Verify the 32/64-bit path selection matches what we want
    if is_64bit:
        # For 64-bit: ensure it actually needs 64-bit (would overflow 32-bit)
        # If it happens to fit in 32-bit, increase coefficient magnitudes
        if can_use_32bit(bits_per_sample, coefs, order, shift):
            # Bump coefficients to force 64-bit path
            scale = 2
            while can_use_32bit(bits_per_sample, coefs, order, shift) and scale < 32:
                coefs = [clamp_int32(c * scale) for c in coefs]
                # Re-bound to 15-bit precision
                coefs = [max(-max_coef * scale, min(max_coef * scale, c)) for c in coefs]
                scale *= 2
    else:
        # For 32-bit: ensure it fits in 32-bit
        while not can_use_32bit(bits_per_sample, coefs, order, shift):
            # Reduce coefficient magnitudes
            coefs = [c // 2 for c in coefs]
            if all(c == 0 for c in coefs):
                coefs[0] = 1  # Avoid degenerate case
                break

    # Compute expected output using Python reference
    if is_64bit or not can_use_32bit(bits_per_sample, coefs, order, shift):
        expected = lpc_restore_64bit(input_buffer, order, coefs, shift)
        actual_64bit = 1
    else:
        expected = lpc_restore_32bit(input_buffer, order, coefs, shift)
        actual_64bit = 0

    return {
        "is_64bit": actual_64bit,
        "order": order,
        "shift": shift,
        "bits_per_sample": bits_per_sample,
        "num_samples": num_samples,
        "coefs": coefs,
        "input": input_buffer,
        "expected": expected,
        "synthetic": True,
    }


def generate_synthetic_vectors(existing_vectors, count_per_group, rng):
    """Generate synthetic vectors to fill coverage gaps."""
    # Determine what's already covered
    existing_groups = set()
    for v in existing_vectors:
        if v["order"] > 0:
            existing_groups.add((v["is_64bit"], v["order"]))

    synthetic = []

    # Generate for orders 1-32, both 32-bit and 64-bit paths
    for order in range(1, 33):
        for is_64bit in [0, 1]:
            key = (is_64bit, order)
            existing_count = sum(1 for v in existing_vectors
                                if (v["is_64bit"], v["order"]) == key)
            needed = max(0, count_per_group - existing_count)
            for _ in range(needed):
                vec = generate_synthetic_vector(order, is_64bit, rng)
                synthetic.append(vec)

    return synthetic


def select_vectors(vectors, max_per_group):
    """Select a diverse subset of vectors covering all orders and both paths."""
    groups = defaultdict(list)
    for i, v in enumerate(vectors):
        if v["order"] == 0:
            continue  # Order 0 is identity (no LPC), assembly doesn't handle it
        key = (v["is_64bit"], v["order"])
        groups[key].append(i)

    selected_indices = []
    for key in sorted(groups.keys()):
        indices = groups[key]
        selected_indices.extend(indices[:max_per_group])

    return [vectors[i] for i in sorted(set(selected_indices))]


def print_coverage(vectors):
    """Print a coverage summary."""
    groups = defaultdict(lambda: {"real": 0, "synthetic": 0})
    for v in vectors:
        key = (v["is_64bit"], v["order"])
        if v.get("synthetic"):
            groups[key]["synthetic"] += 1
        else:
            groups[key]["real"] += 1

    total_real = sum(g["real"] for g in groups.values())
    total_synthetic = sum(g["synthetic"] for g in groups.values())

    print(f"\nSelected {len(vectors)} test vectors ({total_real} real, {total_synthetic} synthetic):")
    print(f"{'Path':<10} {'Order':<8} {'Real':<8} {'Synth':<8}")
    print("-" * 34)
    for (is_64, order), counts in sorted(groups.items()):
        path = "64-bit" if is_64 else "32-bit"
        print(f"{path:<10} {order:<8} {counts['real']:<8} {counts['synthetic']:<8}")

    # Check for gaps
    all_orders = {k[1] for k in groups}
    for o in range(1, 33):
        if o not in all_orders:
            print(f"WARNING: No vectors for order {o}")


def format_array(name, values, per_line=8):
    """Format a C array."""
    lines = [f"static const int32_t {name}[] = {{"]
    for i in range(0, len(values), per_line):
        chunk = values[i:i + per_line]
        formatted = ", ".join(str(v) for v in chunk)
        if i + per_line < len(values):
            formatted += ","
        lines.append(f"    {formatted}")
    lines.append("};")
    return "\n".join(lines)


def generate_header(vectors, output_path):
    """Generate C header file with test vectors."""
    lines = []
    lines.append("// Auto-generated by generate_test_vectors.py - do not edit")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("#include <stddef.h>")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    uint8_t is_64bit;")
    lines.append("    uint32_t order;")
    lines.append("    int32_t shift;")
    lines.append("    uint32_t bits_per_sample;")
    lines.append("    uint32_t num_samples;")
    lines.append("    const int32_t* coefficients;")
    lines.append("    const int32_t* input_buffer;")
    lines.append("    const int32_t* expected_output;")
    lines.append("} LpcTestVector;")
    lines.append("")

    for i, v in enumerate(vectors):
        lines.append(format_array(f"vec_{i}_coefs", v["coefs"]))
        lines.append("")
        lines.append(format_array(f"vec_{i}_input", v["input"]))
        lines.append("")
        lines.append(format_array(f"vec_{i}_expected", v["expected"]))
        lines.append("")

    lines.append("static const LpcTestVector lpc_test_vectors[] = {")
    for i, v in enumerate(vectors):
        lines.append(
            f"    {{{v['is_64bit']}, {v['order']}, {v['shift']}, "
            f"{v['bits_per_sample']}, {v['num_samples']}, "
            f"vec_{i}_coefs, vec_{i}_input, vec_{i}_expected}},"
        )
    lines.append("};")
    lines.append("")
    lines.append("static const size_t lpc_test_vector_count = "
                 "sizeof(lpc_test_vectors) / sizeof(lpc_test_vectors[0]);")
    lines.append("")

    with open(output_path, "w") as f:
        f.write("\n".join(lines))

    print(f"Generated {output_path} with {len(vectors)} test vectors")


def main():
    parser = argparse.ArgumentParser(description="Generate LPC test vector C header")
    parser.add_argument("-i", "--input", required=True, help="Binary dump file")
    parser.add_argument("-o", "--output", required=True, help="Output C header path")
    parser.add_argument("--max-per-group", type=int, default=3,
                        help="Max vectors per (is_64bit, order) group (default: 3)")
    parser.add_argument("--no-synthetic", action="store_true",
                        help="Disable synthetic vector generation")
    parser.add_argument("--seed", type=int, default=42,
                        help="Random seed for synthetic vectors (default: 42)")
    args = parser.parse_args()

    all_vectors = parse_vectors(args.input)
    print(f"Parsed {len(all_vectors)} vectors from {args.input}")

    if not args.no_synthetic:
        rng = random.Random(args.seed)
        synthetic = generate_synthetic_vectors(all_vectors, args.max_per_group, rng)
        print(f"Generated {len(synthetic)} synthetic vectors")
        all_vectors.extend(synthetic)

    selected = select_vectors(all_vectors, args.max_per_group)
    print_coverage(selected)

    generate_header(selected, args.output)


if __name__ == "__main__":
    main()
