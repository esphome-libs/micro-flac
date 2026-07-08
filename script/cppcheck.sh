#!/bin/bash

# Run cppcheck whole-program analysis on first-party sources
#
# Complements clang-tidy: cppcheck sees every first-party source in one pass,
# so its unusedFunction check can flag functions with no caller anywhere in
# the project -- something a per-translation-unit tool cannot do. The scan
# includes host_examples/, tests/, and examples/ so public API entry points
# have visible callers; anything unusedFunction still flags is dead beyond
# the API surface.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

if ! command -v cppcheck &> /dev/null; then
    echo "Error: cppcheck not found (brew install cppcheck / apt-get install cppcheck)"
    exit 1
fi

cd "$ROOT_DIR"

# Suppressions:
#   lib/micro-ogg-demuxer -- submodule, not linted here (matches clang-tidy exclusions)
#   useStlAlgorithm -- raw loops are often clearer; stylistic nag
#   functionStatic on the public header -- FLACStreamInfo accessors are instance
#       methods by API design, matching num_channels()/bits_per_sample()
#   missingInclude* -- system and ESP-IDF headers are not resolvable here;
#       cppcheck analyzes without them
#
# examples/ compiles against ESP-IDF headers cppcheck can't see; that only
# shallows the analysis of those files, it doesn't produce false positives.
cppcheck \
    --enable=warning,style,unusedFunction \
    --std=c++14 \
    --inline-suppr \
    --quiet \
    --error-exitcode=1 \
    --suppress=missingIncludeSystem \
    --suppress=missingInclude \
    --suppress='*:lib/micro-ogg-demuxer/*' \
    --suppress=useStlAlgorithm \
    --suppress='functionStatic:include/micro_flac/*' \
    -i build \
    -i host_examples/flac_to_wav/build \
    -i tests/regression/build \
    -i tests/fuzz/build-libfuzzer \
    -i tests/fuzz/build-standalone \
    -i tests/fuzz/build-cov \
    -i examples/decode_benchmark/.pio \
    -i examples/test_lpc_asm/.pio \
    -I include \
    -I src \
    -I lib/micro-ogg-demuxer/include \
    src \
    host_examples \
    tests \
    examples

echo "cppcheck passed"
