#!/bin/bash

# Run clang-tidy on source files
# Requires a compile_commands.json in the build directory

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${ROOT_DIR}/host_examples/flac_to_wav/build"
WAV_TO_FLAC_BUILD_DIR="${ROOT_DIR}/host_examples/wav_to_flac/build"
ENCODER_TESTS_BUILD_DIR="${ROOT_DIR}/tests/encoder/build"

# Find clang-tidy. A pre-set $CLANG_TIDY (CI pins it to clang-tidy-18) wins over PATH discovery.
CLANG_TIDY="${CLANG_TIDY:-}"
if [ -z "$CLANG_TIDY" ]; then
    for name in clang-tidy clang-tidy-18 clang-tidy-17 clang-tidy-16 clang-tidy-15; do
        if command -v "$name" &> /dev/null; then
            CLANG_TIDY="$name"
            break
        fi
    done
fi

# Check Homebrew LLVM paths on macOS
if [ -z "$CLANG_TIDY" ]; then
    for path in /opt/homebrew/opt/llvm/bin/clang-tidy /usr/local/opt/llvm/bin/clang-tidy; do
        if [ -x "$path" ]; then
            CLANG_TIDY="$path"
            break
        fi
    done
fi

# Validate the resolved binary up front: catches both an empty result and a bogus pre-set
# $CLANG_TIDY, instead of failing later with a bare "command not found".
if ! command -v "$CLANG_TIDY" &> /dev/null; then
    echo "Error: clang-tidy not found or not executable: '${CLANG_TIDY:-unset}'"
    exit 1
fi

# Ensure compile_commands.json exists for each project. Each only knows the
# compile flags for its own sources (flac_to_wav.cpp, wav_to_flac.cpp, or the
# encoder unit tests) plus the shared src/ library, so all three databases are
# needed and merged below.
if [ ! -f "${BUILD_DIR}/compile_commands.json" ]; then
    echo "Generating compile_commands.json (flac_to_wav)..."
    cmake -B "$BUILD_DIR" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "${ROOT_DIR}/host_examples/flac_to_wav"
fi
if [ ! -f "${WAV_TO_FLAC_BUILD_DIR}/compile_commands.json" ]; then
    echo "Generating compile_commands.json (wav_to_flac)..."
    cmake -B "$WAV_TO_FLAC_BUILD_DIR" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "${ROOT_DIR}/host_examples/wav_to_flac"
fi
if [ ! -f "${ENCODER_TESTS_BUILD_DIR}/compile_commands.json" ]; then
    echo "Generating compile_commands.json (encoder tests)..."
    cmake -B "$ENCODER_TESTS_BUILD_DIR" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENABLE_SANITIZERS=OFF \
        "${ROOT_DIR}/tests/encoder"
fi

# Merge the compile_commands.json databases into one directory. Pointing -p at
# any one of them leaves the other projects' sources with no compile command
# (clang-tidy falls back to a generic command line and can't resolve their -I
# path to src/, e.g. "flac_format.h not found"). Entries are deduplicated by
# file path; src/*.cpp appears in every database with equivalent flags, so
# any copy is fine.
MERGED_DIR="${ROOT_DIR}/build/clang-tidy-cdb"
mkdir -p "$MERGED_DIR"
python3 -c "
import json, sys

entries = []
seen = set()
for path in sys.argv[1:]:
    with open(path) as f:
        for entry in json.load(f):
            if entry['file'] in seen:
                continue
            seen.add(entry['file'])
            entries.append(entry)

with open('$MERGED_DIR/compile_commands.json', 'w') as f:
    json.dump(entries, f, indent=2)
" "${BUILD_DIR}/compile_commands.json" "${WAV_TO_FLAC_BUILD_DIR}/compile_commands.json" \
    "${ENCODER_TESTS_BUILD_DIR}/compile_commands.json"

# Find all source files, excluding build/ and build-*/ directories (variant
# build trees, as .gitignore ignores them)
# Note: examples/ excluded as ESP-IDF code can't be checked without ESP-IDF headers
SOURCES=$(find "$ROOT_DIR/src" "$ROOT_DIR/host_examples" "$ROOT_DIR/tests/encoder" \
    -type d \( -path '*/build' -o -path '*/build-*' \) -prune -o \
    \( -name '*.cpp' -o -name '*.c' \) -print 2>/dev/null || true)

if [ -z "$SOURCES" ]; then
    echo "No source files found"
    exit 0
fi

# Parse arguments
FIX_FLAG=""
if [ "$1" = "--fix" ]; then
    FIX_FLAG="--fix"
fi

echo "Running clang-tidy..."
# --warnings-as-errors keeps the exit code non-zero on any finding even if a
# repo's .clang-tidy ever loses its WarningsAsErrors line; CI relies on this.
$CLANG_TIDY -p "$MERGED_DIR" --warnings-as-errors='*' $FIX_FLAG $SOURCES
