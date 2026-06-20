#!/usr/bin/env bash
# Generate a seed corpus for fuzz_flac_decode.
#
# Outputs:
#   seeds_flac/   FLAC seeds covering blocksizes, bit depths, sample rates,
#                 channel counts, prediction modes, and malformed headers,
#                 sourced from the conformance suite under test/flac-test-files/.
#                 Each native "fLaC" seed is also remuxed to Ogg FLAC (.oga) so
#                 the OggS demuxer path is seeded too.
#
# The conformance files are the seed material: the FLAC project hand-built them to
# span the format's corners (every blocksize, qlp precision, partition order,
# wasted bits, variable blocksize, 8/12/15/20/24/32-bit, 768 kHz, plus a faulty/
# set of deliberately broken streams). Remuxing with `-c:a copy` keeps the exact
# FLAC bitstream and only rewraps it in Ogg, so every native edge case carries over
# to the Ogg container path.
#
# Requires: ffmpeg on PATH (only for the Ogg remux; native seeds are plain copies).
#
# After running this, MINIMIZE into the live corpus before fuzzing:
#   mkdir -p corpus_flac
#   ./build-libfuzzer/fuzz_flac_decode -merge=1 corpus_flac/ seeds_flac/
# -merge=1 keeps only inputs that add coverage against this harness.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"

test_dir="$here/../../test/flac-test-files"
if [[ ! -d "$test_dir" ]]; then
    echo "error: conformance files not found at $test_dir" >&2
    exit 1
fi

have_ffmpeg=1
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "[seeds] warning: ffmpeg not on PATH; skipping Ogg FLAC remux (native seeds only)" >&2
    have_ffmpeg=0
fi

rm -rf seeds_flac
mkdir -p seeds_flac

echo "[seeds] copying conformance files + remuxing to Ogg FLAC..."

native_count=0
ogg_count=0
for cat in subset uncommon faulty; do
    dir="$test_dir/$cat"
    [[ -d "$dir" ]] || continue
    for f in "$dir"/*.flac; do
        [[ -e "$f" ]] || continue
        base="$(basename "$f")"
        # The conformance names start with a unique two-digit index per category.
        idx="${base:0:2}"
        native="seeds_flac/${cat}${idx}.flac"
        cp "$f" "$native"
        native_count=$((native_count + 1))

        # Remux the SAME bitstream into Ogg FLAC. Faulty streams ffmpeg cannot
        # parse are skipped (they stay native-only seeds).
        if [[ "$have_ffmpeg" -eq 1 ]]; then
            if ffmpeg -hide_banner -loglevel error -y -i "$f" -c:a copy -f ogg \
                "seeds_flac/${cat}${idx}.oga" >/dev/null 2>&1; then
                ogg_count=$((ogg_count + 1))
            else
                rm -f "seeds_flac/${cat}${idx}.oga"
            fi
        fi
    done
done

echo "[seeds] $native_count native + $ogg_count Ogg FLAC base seeds"

# ---------------------------------------------------------------------------
# Fuzzer config tails.
#
# fuzz_flac_decode reads its configuration from the BACK of each input
# (FuzzedDataProvider): one cfg byte, then up to 64 chunk-control bytes. A bare
# seed therefore loses ~65 bytes off its tail to those reads. Appending a config
# tail keeps the ENTIRE seed intact as decoder payload while still giving
# libFuzzer a mutable region for the chunk sizes and feature flags. A few
# variants pre-set cfg so the CRC / 32-bit-output / metadata / replay paths are
# seeded directly rather than found by mutation.
#
# cfg layout (matches the harness): bit0 = enable CRC, bit1 = 32-bit output,
#   bit2 = store metadata, bit3 = replay across reset(), bits4-5 = native output
#   alignment offset. When bit2 is set the harness also reads 7 per-type metadata
#   size-limit bytes from the tail (one per block type); the tails below emit them
#   as 0xFF so flagged seeds store every block.
# ---------------------------------------------------------------------------

# Emit one raw byte from a decimal value. Octal escape keeps this portable to
# macOS's stock bash 3.2 (whose printf lacks \xHH).
emit_byte() { printf "\\$(printf '%03o' "$1")"; }

# Append a config tail: 64 neutral chunk-control bytes (== the harness
# MAX_CONTROL_BYTES, so every control byte comes from the pad and none is peeled
# off the real stream), then 7 store-everything metadata bytes when bit2 is set,
# then the cfg byte. Consumption order off the back is cfg, metadata, chunk-control,
# so file order is chunk-control, metadata, cfg.
append_config_tail() {
    local f=$1 cfg=$2 i
    {
        for ((i = 0; i < 64; i++)); do emit_byte 32; done  # ~1 KiB chunks (1 + 32*32)
        if (( (cfg & 0x04) != 0 )); then
            for ((i = 0; i < 7; i++)); do emit_byte 255; done  # store every metadata block
        fi
        emit_byte "$cfg"
    } >> "$f"
}

# Copy a pristine base seed and give the copy a specific config tail.
#   $1 base  $2 dstname  $3 cfg
mkvariant() {
    local src=$1 dst=$2 cfg=$3
    [[ -f "$src" ]] || return 0
    cp "$src" "seeds_flac/$dst"
    append_config_tail "seeds_flac/$dst" "$cfg"
}

echo "[seeds] appending fuzzer config tails"

# Snapshot the pristine bases before adding variants, so variants are not
# double-tailed by the pass below.
base_list="$(mktemp)"
trap 'rm -f "$base_list"' EXIT
find seeds_flac -maxdepth 1 -type f \( -name '*.flac' -o -name '*.oga' \) | sort > "$base_list"

# Feature-flag variants, seeded off a clean 16-bit stereo blocksize-4096 base.
st=seeds_flac/subset01.flac
mkvariant "$st" cfg_crc_on.flac    1     # CRC validation on
mkvariant "$st" cfg_out32.flac     2     # 32-bit int32_t* output overload
mkvariant "$st" cfg_metadata.flac  4     # store metadata blocks
mkvariant "$st" cfg_replay.flac    8     # replay across reset()
mkvariant "$st" cfg_all.flac      31     # CRC + 32-bit + metadata + replay
# Same flag spread on an Ogg FLAC base, when ffmpeg produced one.
mkvariant seeds_flac/subset01.oga  cfg_ogg_all.oga 31

# Every pristine base: neutral tail (cfg=0) so the full seed survives as payload
# while libFuzzer still gets a mutable control region.
while IFS= read -r f; do
    append_config_tail "$f" 0
done < "$base_list"

echo "[seeds] $(ls seeds_flac | wc -l | tr -d ' ') total seeds generated in seeds_flac/"
echo "[seeds] next: minimize into corpus_flac/ with -merge=1 (see header / README)"
echo "[seeds] done"
