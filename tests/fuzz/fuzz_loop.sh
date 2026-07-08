#!/usr/bin/env bash
# Continuously fuzz fuzz_flac_decode in fixed-length cycles, minimizing the
# corpus after each cycle so it never grows without bound, and logging the
# corpus's source coverage after each minimization.
#
# Each cycle:
#   1. Fuzz for FUZZ_TIME seconds (default 600 = 10 min) with -jobs/-workers
#      parallelism, growing corpus_flac/ with new coverage-increasing inputs.
#   2. Coverage-merge corpus_flac/ into a fresh minimized set, then swap it
#      back in, dropping every input that no longer adds coverage.
#   3. Run coverage.sh on the minimized corpus and log the TOTAL region/
#      function/line/branch percentages (skip with --no-coverage). A timestamped
#      line is also appended to fuzz_loop_coverage.log for run history.
#
# The merge keeps disk usage bounded on multi-hour runs: without it, corpus_flac/
# accrues redundant inputs indefinitely and can fill the disk.
#
# Loops until interrupted (Ctrl-C) or a crash is found. On a crash libFuzzer
# drops crash-<sha> in this directory and the loop stops so it can be triaged.
#
# Usage:
#   ./fuzz_loop.sh                 # 10-min cycles, loop forever
#   ./fuzz_loop.sh --time=300      # 5-min cycles (300s)
#   ./fuzz_loop.sh --cycles=6      # stop after 6 cycles, then exit
#   ./fuzz_loop.sh --no-coverage   # skip the coverage measurement each cycle
#
# Requires: build-libfuzzer/fuzz_flac_decode (see README "Build") and a populated
# corpus_flac/ (see README "Seed corpus"). Coverage needs an LLVM toolchain
# (see coverage.sh); without it, pass --no-coverage.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"

fuzzer="./build-libfuzzer/fuzz_flac_decode"
corpus="corpus_flac"
dict="flac.dict"
cov_log="fuzz_loop_coverage.log"
fuzz_time=600
cycles=0          # 0 = unlimited
run_coverage=1

for arg in "$@"; do
    case "$arg" in
        --time=*)      fuzz_time="${arg#--time=}" ;;
        --cycles=*)    cycles="${arg#--cycles=}" ;;
        --no-coverage) run_coverage=0 ;;
        -h|--help)
            sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) echo "error: unknown arg $arg" >&2; exit 1 ;;
    esac
done

if [[ ! -x "$fuzzer" ]]; then
    echo "error: $fuzzer not found or not executable" >&2
    echo "       build it first (see README \"Build\")" >&2
    exit 1
fi
if [[ ! -d "$corpus" ]]; then
    echo "error: corpus directory '$corpus' not found" >&2
    echo "       seed it first (see README \"Seed corpus\")" >&2
    exit 1
fi

cycle=0
while (( cycles == 0 || cycle < cycles )); do
    cycle=$(( cycle + 1 ))
    echo "=== cycle $cycle: fuzzing for ${fuzz_time}s ($(date)) ==="

    # -max_total_time bounds each parallel job, so all 8 stop together at
    # ~fuzz_time wall-clock and exit cleanly, leaving the grown corpus on disk.
    # (timeout(1) on the whole thing would orphan the worker children instead.)
    if ! "$fuzzer" -dict="$dict" -jobs=8 -workers=8 -verbosity=0 \
            -max_len=65536 -max_total_time="$fuzz_time" "$corpus/"; then
        echo "=== fuzzer exited non-zero (crash or interrupt); stopping ===" >&2
        echo "    check for crash-* / leak-* / timeout-* reproducers in $here" >&2
        exit 1
    fi

    echo "=== cycle $cycle: minimizing corpus ($(date)) ==="
    rm -rf corpus_min
    mkdir -p corpus_min
    "$fuzzer" -merge=1 corpus_min "$corpus"
    rm -rf "$corpus"
    mv corpus_min "$corpus"

    corpus_files="$(find "$corpus" -type f | wc -l | tr -d ' ')"
    echo "=== cycle $cycle: corpus now $corpus_files files ==="

    if (( run_coverage )); then
        echo "=== cycle $cycle: measuring coverage ($(date)) ==="
        if cov_output="$(./coverage.sh 2>&1)"; then
            # llvm-cov's TOTAL row exposes region/function/line/branch coverage
            # as its %-suffixed fields, in that order.
            total_line="$(printf '%s\n' "$cov_output" | grep -E '^TOTAL' || true)"
            pcts=($(awk '{for (i=1;i<=NF;i++) if ($i ~ /%$/) print $i}' <<<"$total_line"))
            if (( ${#pcts[@]} >= 3 )); then
                summary="lines ${pcts[2]}, functions ${pcts[1]}, regions ${pcts[0]}"
                (( ${#pcts[@]} >= 4 )) && summary+=", branches ${pcts[3]}"
                echo "=== cycle $cycle: coverage — $summary ==="
                printf '%s\tcycle %d\t%s files\t%s\n' \
                    "$(date '+%Y-%m-%d %H:%M:%S')" "$cycle" "$corpus_files" "$summary" \
                    >> "$cov_log"
            else
                echo "warning: could not parse coverage TOTAL line; continuing" >&2
            fi
        else
            echo "warning: coverage.sh failed; continuing fuzzing (last lines:)" >&2
            printf '%s\n' "$cov_output" | tail -n 5 >&2
        fi
    fi
done
