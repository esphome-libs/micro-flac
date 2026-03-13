#!/usr/bin/env python3
"""
Convert a FLAC file to a C header file with embedded byte array.

Usage:
    python convert_flac.py -i input.flac -o output.h -v variable_name

Example:
    python convert_flac.py -i eroica_clip.flac -o src/test_audio_flac.h -v test_audio_flac_data
"""

import argparse
import os
import sys


def convert_flac_to_header(input_path: str, output_path: str, variable_name: str) -> None:
    """Convert a FLAC file to a C header with embedded byte array."""

    # Read the binary FLAC file
    with open(input_path, 'rb') as f:
        data = f.read()

    file_size = len(data)
    print(f"Input file: {input_path}")
    print(f"File size: {file_size} bytes ({file_size / 1024:.1f} KB)")

    # Generate the header file
    header_guard = variable_name.upper() + "_H"

    with open(output_path, 'w') as f:
        f.write(f"// Auto-generated from {os.path.basename(input_path)}\n")
        f.write(f"// Size: {file_size} bytes\n\n")
        f.write(f"#ifndef {header_guard}\n")
        f.write(f"#define {header_guard}\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"const uint32_t {variable_name}_len = {file_size};\n\n")
        f.write(f"const uint8_t {variable_name}[] = {{\n")

        # Write bytes, 16 per line for readability
        bytes_per_line = 16
        for i in range(0, file_size, bytes_per_line):
            chunk = data[i:i + bytes_per_line]
            hex_values = ", ".join(f"0x{b:02x}" for b in chunk)

            if i + bytes_per_line < file_size:
                f.write(f"    {hex_values},\n")
            else:
                f.write(f"    {hex_values}\n")

        f.write("};\n\n")
        f.write(f"#endif // {header_guard}\n")

    print(f"Output file: {output_path}")
    print("Conversion complete!")


def main():
    parser = argparse.ArgumentParser(
        description="Convert a FLAC file to a C header with embedded byte array"
    )
    parser.add_argument(
        "-i", "--input",
        required=True,
        help="Input FLAC file path"
    )
    parser.add_argument(
        "-o", "--output",
        required=True,
        help="Output C header file path"
    )
    parser.add_argument(
        "-v", "--variable",
        default="flac_data",
        help="Variable name for the byte array (default: flac_data)"
    )

    args = parser.parse_args()

    if not os.path.exists(args.input):
        print(f"Error: Input file '{args.input}' not found", file=sys.stderr)
        sys.exit(1)

    # Create output directory if needed
    output_dir = os.path.dirname(args.output)
    if output_dir and not os.path.exists(output_dir):
        os.makedirs(output_dir)

    convert_flac_to_header(args.input, args.output, args.variable)


if __name__ == "__main__":
    main()
