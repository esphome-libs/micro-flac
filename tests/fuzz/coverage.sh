#!/usr/bin/env bash
# Measure source-level coverage of the fuzzer corpus.
#
# Builds fuzz_flac_decode with clang's source-based coverage instrumentation,
# replays the corpus through it once (libFuzzer -runs=0), and prints a
# per-function coverage report. Pass --html to additionally render a browsable
# line-by-line report to cov-html/.
#
# Requires: an LLVM toolchain with llvm-profdata and llvm-cov.
#   - macOS: Homebrew LLVM (`brew install llvm`); Apple clang's llvm-cov
#     tooling is incomplete for this workflow.
#   - Linux: the system clang/llvm packages are sufficient.
# Override autodetection by exporting LLVM_PREFIX (the dir whose bin/ holds
# clang, clang++, llvm-profdata, and llvm-cov).

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"

corpus="corpus_flac"
build_html=0
for arg in "$@"; do
    case "$arg" in
        --html) build_html=1 ;;
        --corpus=*) corpus="${arg#--corpus=}" ;;
        -h|--help)
            sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) echo "error: unknown arg $arg" >&2; exit 1 ;;
    esac
done

if [[ ! -d "$corpus" ]]; then
    echo "error: corpus directory '$corpus' not found" >&2
    echo "       run ./generate_seeds.sh and merge seeds_flac/ -> $corpus/" >&2
    exit 1
fi

# Locate the LLVM tools. Priority: explicit $LLVM_PREFIX, then Homebrew's
# llvm (macOS), then whatever is on PATH (typical on Linux).
if [[ -n "${LLVM_PREFIX:-}" ]]; then
    llvm_prefix="$LLVM_PREFIX"
elif command -v brew >/dev/null 2>&1 && brew --prefix llvm >/dev/null 2>&1; then
    llvm_prefix="$(brew --prefix llvm)"
else
    llvm_prefix=""
fi

if [[ -n "$llvm_prefix" ]]; then
    clang="$llvm_prefix/bin/clang"
    clangxx="$llvm_prefix/bin/clang++"
    profdata="$llvm_prefix/bin/llvm-profdata"
    llvmcov="$llvm_prefix/bin/llvm-cov"
else
    # Fall back to PATH (system LLVM, common on Linux).
    clang="$(command -v clang || true)"
    clangxx="$(command -v clang++ || true)"
    profdata="$(command -v llvm-profdata || true)"
    llvmcov="$(command -v llvm-cov || true)"
fi

for spec in "clang:$clang" "clang++:$clangxx" "llvm-profdata:$profdata" "llvm-cov:$llvmcov"; do
    name="${spec%%:*}"
    path="${spec#*:}"
    if [[ -z "$path" || ! -x "$path" ]]; then
        echo "error: required LLVM tool '$name' not found" >&2
        echo "       install LLVM (macOS: 'brew install llvm') or set LLVM_PREFIX to its prefix" >&2
        echo "       on Debian/Ubuntu the tools may be versioned (e.g. llvm-cov-18);" >&2
        echo "       point LLVM_PREFIX at a dir whose bin/ has unversioned names" >&2
        exit 1
    fi
done

# Coverage-only build dir (kept separate from build-libfuzzer/ so the
# normal fuzzing build doesn't pick up the instrumentation overhead).
cov_flags="-fprofile-instr-generate -fcoverage-mapping"

echo "[cov] configuring build-cov/"
cmake -B build-cov \
    -DCMAKE_C_COMPILER="$clang" \
    -DCMAKE_CXX_COMPILER="$clangxx" \
    -DCMAKE_C_FLAGS="$cov_flags" \
    -DCMAKE_CXX_FLAGS="$cov_flags" \
    -DCMAKE_EXE_LINKER_FLAGS="$cov_flags" \
    . >/dev/null

echo "[cov] building fuzz_flac_decode"
cmake --build build-cov --target fuzz_flac_decode >/dev/null

binary="build-cov/fuzz_flac_decode"

echo "[cov] replaying $(find "$corpus" -type f | wc -l | tr -d ' ') corpus inputs"
rm -f cov.profraw cov.profdata
LLVM_PROFILE_FILE="cov.profraw" "./$binary" -runs=0 "$corpus" >/dev/null 2>&1

"$profdata" merge -sparse cov.profraw -o cov.profdata

# Restrict the report to microFLAC sources. The Ogg demuxer submodule and the
# harness itself aren't what we're auditing here.
ignore_re='(lib/micro-ogg-demuxer|tests/fuzz)'

echo
"$llvmcov" report "$binary" \
    -instr-profile=cov.profdata \
    -ignore-filename-regex="$ignore_re"

if [[ $build_html -eq 1 ]]; then
    echo
    echo "[cov] rendering HTML to cov-html/"
    rm -rf cov-html
    "$llvmcov" show "$binary" \
        -instr-profile=cov.profdata \
        -ignore-filename-regex="$ignore_re" \
        -format=html -output-dir=cov-html \
        -show-line-counts-or-regions >/dev/null
    echo "[cov] open cov-html/index.html"
fi
