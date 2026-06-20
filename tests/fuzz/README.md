# microFLAC fuzzer

libFuzzer harness for the FLAC decoder.

- `fuzz_flac_decode` drives `FLACDecoder::decode()` with raw FLAC bytes (native
  `fLaC` or Ogg FLAC `OggS`) in variably-sized chunks. It covers container
  auto-detection, the Ogg demuxer, metadata parsing, frame-header accumulation,
  and the bitstream decoder (fixed/LPC prediction, Rice residual coding, stereo
  decorrelation, PCM packing).

## Decoder configuration coverage

The harness also varies how the decoder is called, reaching the two output
overloads, CRC validation, metadata storage, and the re-stream path. Configuration
comes from the tail of the input: `FuzzedDataProvider` reads integrals off the
back, so only the trailing config/control bytes are peeled and the front of a real
seed stays intact.

- One config byte: bit 0 enables CRC validation; bit 1 selects the 32-bit
  `int32_t*` output overload (left-justified) over the native byte-packed
  `uint8_t*` overload; bit 2 stores metadata blocks (else they are skipped); bit 3
  replays the whole stream a second time across a `reset()`; bits 4-5 set the
  native output buffer's base alignment offset (0..3 bytes).
- When bit 2 is set, seven more tail bytes follow, one per configurable block type
  (PADDING through PICTURE, plus the unknown-type bucket). Each maps to a size
  limit: 0 skips that type, 0xFF stores it regardless of size, and the rest pick a
  1 B..16 MiB threshold. This gives the fuzzer per-type control over the store/skip
  decision and the size boundary, not just all-or-nothing storage. STREAMINFO is
  always parsed and never stored as a block, so it takes no limit.
- The rest of the reserved tail is chunk-control bytes (up to 64): each sets one
  input chunk size (1..8161 bytes), splitting frame headers, subframes, and Rice
  partitions across `decode()` calls.
- An exhausted provider reads 0, the default: native output, CRC off, no metadata,
  single pass, aligned. CRC defaults off so a mutated frame's CRC mismatch does not
  abort decoding, letting the decoder reach deeper into the prediction/residual
  paths. The `cfg_crc_on.*` seed and bit 0 cover the validation path itself.

The replay bit covers the re-stream path a looping caller hits: `reset()` frees
and re-allocates the decoded-block buffer (and any Ogg demuxer), then a second
`HEADER_READY` fires. The decoded output matches the first pass, so it adds little
line coverage; the point is the free/realloc/decode ordering under ASan, which a
single pass never reaches. Gating it on a bit keeps non-replay inputs at full
throughput.

On every decode the harness asserts a set of **Tier 1 structural invariants**
(single-decode, no reference needed): `bytes_consumed` never exceeds the input
handed in; once `HEADER_READY`, the channel count is 1..8, the bit depth 1..32
(bytes-per-sample 1..4), and the decoded sample count is both a multiple of the
channel count and no larger than `get_output_buffer_size_samples()`; and a single
`decode()` never reports more output than the buffer holds. A violation aborts,
surfacing it like any sanitizer finding.

## Requirements

- A Clang with the libFuzzer runtime.
  - **macOS:** `brew install llvm`: Apple's stock clang omits the libFuzzer
    runtime, so the Homebrew build is required.
  - **Linux:** the system `clang++` already ships libFuzzer; no extra install.
- ffmpeg on `PATH` for the Ogg FLAC seed remux (native seeds are plain copies and
  need no ffmpeg).

The build commands below use `$CLANGXX` for the compiler. Point it at the right
Clang for your platform:

```sh
export CLANGXX=$(brew --prefix llvm)/bin/clang++   # macOS / Homebrew LLVM
export CLANGXX=clang++                             # Linux / system clang
```

## Build

```sh
cd tests/fuzz
cmake -B build-libfuzzer -DCMAKE_CXX_COMPILER="$CLANGXX" .
cmake --build build-libfuzzer
```

For crash reproducers without libFuzzer:

```sh
cmake -B build-standalone -DFUZZ_USE_LIBFUZZER=OFF -DCMAKE_CXX_COMPILER="$CLANGXX" .
cmake --build build-standalone
./build-standalone/fuzz_flac_decode path/to/crashing.flac   # repro one input
./build-standalone/fuzz_flac_decode                         # torture battery
./build-standalone/fuzz_flac_decode -mutate seed.flac       # ad-hoc mutation run
```

## Seed corpus

The seeds come from the FLAC conformance suite under `test/flac-test-files/`,
hand-built by the FLAC project to span the format's corners (every blocksize, qlp
precision, partition order, wasted bits, variable blocksize, 8/12/15/20/24/32-bit,
up to 768 kHz, 1-8 channels, plus a `faulty/` set of deliberately broken streams).
`generate_seeds.sh` copies each as a native seed and remuxes it to Ogg FLAC with
`ffmpeg -c:a copy`, which keeps the exact FLAC bitstream and only rewraps it, so
every native edge case carries over to the OggS demuxer path.

```sh
./generate_seeds.sh                 # creates seeds_flac/ (native + .oga)
mkdir -p corpus_flac
./build-libfuzzer/fuzz_flac_decode -merge=1 corpus_flac/ seeds_flac/
```

The `-merge=1` step minimizes the corpus: it replays every seed and keeps only the
ones that add coverage against this harness, so `corpus_flac/` starts lean before
the real runs. Always minimize before fuzzing.

Each generated seed gets a **config tail** appended (see "Decoder configuration
coverage" above): because the harness consumes its config/control bytes from the
back of the input, a bare seed would lose its final ~72 bytes to those reads. The
tail is a neutral chunk-control region plus one `cfg` byte (and, for
metadata-enabled variants, seven store-everything per-type bytes), so the whole
stream survives as decoder payload while libFuzzer still has a mutable region to
flip the flags. A few `cfg_*.{flac,oga}` variants pre-set `cfg` (CRC on, 32-bit
output, metadata, replay) so those paths are seeded directly rather than found by
mutation.

Seeds and the live corpus are local-only: the repo-wide `*.flac` gitignore
pattern keeps seeds out of the tree, and `corpus_flac/` is explicitly ignored so
libFuzzer can grow it without polluting `git status`. Regenerate any time with
`./generate_seeds.sh`.

### Merging an external corpus

Any directory of `.flac`/`.oga` files can be folded in with a coverage-guided
merge. OSS-Fuzz, for example, has fuzzed upstream libFLAC for years and publishes
its accumulated corpus. Point the merge at the downloaded directory:

```sh
./build-libfuzzer/fuzz_flac_decode -merge=1 -max_len=1048576 corpus_flac/ /path/to/external/
```

`-merge=1` keeps only inputs that add new coverage against this harness. If the
merge hits a crashing input, libFuzzer writes `crash-<sha>` to the cwd, restarts,
and continues, so it is safe to run on a dirty corpus.

## Run

```sh
./build-libfuzzer/fuzz_flac_decode -dict=flac.dict corpus_flac/
```

Useful flags: `-max_total_time=60`, `-jobs=4`, `-workers=4`,
`-max_len=1048576`, `-rss_limit_mb=4096`.

## UBSan

The build keeps ASan and **all of UBSan** on (`-fsanitize=address,undefined
-fno-sanitize-recover=all`), including the `shift` and bounds checks that catch
real bitstream bugs. The FLAC decoder is written to be UBSan-clean under fuzzing:

- Channel decorrelation and PCM packing route every wrap through `uint32_t`
  helpers (`src/wrapping_arithmetic.h`), so they never trip
  `signed-integer-overflow`.
- The LPC/fixed prediction restore functions, whose intentional modular-int32
  arithmetic the C standard calls UB (the same pattern as upstream
  `FLAC__lpc_restore_signal`), carry
  `__attribute__((no_sanitize("signed-integer-overflow")))`. That attribute only
  engages when `FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION` is defined, which the
  fuzzer `CMakeLists.txt` does. No other UBSan check is disabled, so no blanket
  suppression can hide a finding.

## Corpus coverage

To see which functions in `src/` the saved corpus reaches:

```sh
./coverage.sh           # per-function report on stdout
./coverage.sh --html    # also write cov-html/ for line-by-line browsing
```

The script builds a separate `build-cov/` with clang source-based coverage
instrumentation, replays `corpus_flac/` once via libFuzzer's `-runs=0` mode, and
renders the report with `llvm-cov`. Functions at 0% are codepaths the corpus
isn't reaching, candidates for new seeds or dict entries.

## When a crash is found

1. libFuzzer drops `crash-<sha>` in the current directory.
2. Minimize: `./build-libfuzzer/fuzz_flac_decode -minimize_crash=1 -runs=10000 crash-<sha>`.
3. Reproduce under the standalone binary for cleaner stack traces:
   `./build-standalone/fuzz_flac_decode crash-<sha>`.
4. Keep the reproducer in `crashes/`; once the fix lands and the input no longer
   reproduces, move it to `crashes/fixed_verified/`. Crash inputs are local-only
   (the repo-wide `crash-*` gitignore pattern keeps them out of the tree). Replay
   them after decoder changes for regression cover:

   ```sh
   ./build-libfuzzer/fuzz_flac_decode -runs=0 crashes/
   ```
