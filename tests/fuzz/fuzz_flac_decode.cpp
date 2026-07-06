// Copyright 2026 Kevin Ahrendt
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Fuzz harness for micro_flac::FLACDecoder.
//
// Feeds FLAC bytes (native "fLaC" or Ogg FLAC "OggS") to the streaming decoder in
// variably-sized chunks. That covers the chunk-boundary handling (container
// detection, metadata staging, frame-header and subframe/residual resume) and the
// bitstream decoder (fixed/LPC prediction, Rice coding, stereo decorrelation, PCM
// packing).
//
// A config byte read from the tail of the input leaves the front an intact FLAC
// payload while selecting decode options: the output overload (native uint8_t* or
// 32-bit int32_t*), CRC checking, metadata storage, reset()/replay, and output
// alignment. run_decode_pass asserts the Tier 1 invariants on every decode().
//
// Two build modes:
//   1. libFuzzer:  -fsanitize=fuzzer,address,undefined exposes
//      LLVMFuzzerTestOneInput. Run against a corpus directory:
//          ./fuzz_flac_decode corpus_flac/
//   2. Standalone: define FUZZ_STANDALONE. Takes file paths on argv for crash
//      reproduction, or runs a torture battery with no args.

#include "micro_flac/flac_decoder.h"
#include <fuzzer/FuzzedDataProvider.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using micro_flac::FLACDecoder;
using micro_flac::FLACDecoderResult;
using micro_flac::FLACMetadataType;
using micro_flac::FLACStreamInfo;

// Disable ASan's container-overflow check for this binary. libFuzzer's prebuilt
// runtime (e.g. Homebrew LLVM's libclang_rt.fuzzer) is compiled without libc++
// container annotations. This harness instantiates std::vector<uint8_t>, the same
// type libFuzzer's dictionary parser uses, but with annotations under
// -fsanitize=address. ODR-merging the two at link time leaves the poisoning
// mismatched, so libFuzzer's ParseOneDictionaryEntry false-positives while loading
// a -dict file. The decoder uses raw arrays and the harness sizes its output
// buffers exactly, so a real over-write still lands in ASan's heap red zone.
// ASAN_OPTIONS overrides this default.
// NOLINTNEXTLINE(bugprone-reserved-identifier,readability-identifier-naming): fixed ASan hook name
extern "C" const char* __asan_default_options() {
    return "detect_container_overflow=0";
}

// Upper bound on the chunk-size control bytes pulled from the tail (via
// FuzzedDataProvider, alongside the config byte). They set the per-call chunk
// sizes; reading them off the back keeps the front payload intact, so libFuzzer
// mutates decoder input rather than control bytes.
static constexpr size_t MAX_CONTROL_BYTES = 64;

// Cap on the per-stream output buffer. STREAMINFO bounds max_block_size to 16 bits
// (<=65535) and num_channels to 8, so a real header needs at most ~524k samples
// (~2 MiB at 4 bytes). The cap only matters for a bogus header the decoder still
// accepted, bounding the allocation that header would request.
static constexpr size_t MAX_OUTPUT_SAMPLES = 1UL << 21;

// Map a chunk-control byte to an input chunk size: 1..8161. The low end splits a
// frame header across calls (forcing header/subframe/residual resume); the high
// end exceeds a typical frame. Control bytes cycle for a short ctrl buffer.
static size_t ctrl_to_chunk(uint8_t b) {
    return 1 + static_cast<size_t>(b) * 32;
}

// Number of per-type metadata size-limit bytes pulled from the tail when the
// store-metadata cfg bit is set: one each for PADDING(1)..PICTURE(6) plus the
// unknown-type bucket. STREAMINFO is always parsed and never stored, so it takes
// no limit.
static constexpr size_t MAX_META_BYTES = 7;

// Span of distinct size thresholds the non-sentinel bytes cycle through: shifts
// 0..META_SHIFT_SPAN-1 give limits of 1 B up to 16 MiB, covering the 24-bit max
// length of a metadata block.
static constexpr uint32_t META_SHIFT_SPAN = 25;

// Map a metadata-control byte to a max-size limit for one block type: 0 skips the
// type, 0xFF stores it regardless of size, and the rest pick a threshold so a
// block is stored or skipped depending on whether its declared length crosses it.
static uint32_t ctrl_to_meta_size(uint8_t b) {
    if (b == 0) {
        return 0;
    }
    if (b == 0xFF) {
        return UINT32_MAX;
    }
    return 1U << (static_cast<uint32_t>(b) % META_SHIFT_SPAN);
}

// One streaming pass: feed `payload` to `decoder` in control-byte-sized chunks,
// asserting the Tier 1 oracle on every decode. Factored out so it can run twice
// across a reset() to cover the re-stream path (block buffer free+realloc, Ogg
// demuxer free+realloc, second HEADER_READY) that a single pass misses.
// `out8`/`out32` grow once HEADER_READY sizes them and are reused across passes.
//
// cfg-derived knobs:
//   output_32bit  pick the int32_t* overload (32-bit left-justified) over the
//                 native byte-packed uint8_t* overload.
//   out_align     base-pointer byte offset for the native output buffer (0..3),
//                 so the PCM packing paths run against unaligned destinations.
static void run_decode_pass(FLACDecoder& decoder, const std::vector<uint8_t>& payload,
                            const std::vector<uint8_t>& ctrl, bool output_32bit, uint8_t out_align,
                            std::vector<uint8_t>& out8, std::vector<int32_t>& out32) {
    size_t offset = 0;
    size_t ctrl_idx = 0;
    size_t iterations = 0;
    bool header_ready = false;

    // Each consuming iteration advances `offset` by >= 1 byte (the post-header
    // resync path advances by max(consumed, 1)); the *2 + constant covers the
    // non-advancing header-staging calls and any flush pairing. Either way the
    // loop is finite.
    const size_t max_iterations = payload.size() * 2 + 4096;

    while (offset < payload.size() && iterations < max_iterations) {
        ++iterations;

        size_t chunk_size = ctrl_to_chunk(ctrl[ctrl_idx++ % ctrl.size()]);
        if (chunk_size > payload.size() - offset) {
            chunk_size = payload.size() - offset;
        }

        // Output buffer: nullptr/0 until HEADER_READY (the decoder writes no audio
        // while detecting the container and parsing metadata), then the exact frame
        // size so ASan red-zones any over-write. The native buffer is offset by
        // out_align to drive the unaligned PCM packing destinations.
        uint8_t* out_ptr8 = nullptr;
        int32_t* out_ptr32 = nullptr;
        size_t out_capacity = 0;  // bytes (native) or samples (32-bit)
        if (header_ready) {
            if (output_32bit) {
                out_ptr32 = out32.data();
                out_capacity = out32.size();
            } else {
                out_ptr8 = out8.data() + out_align;
                out_capacity = out8.size() - out_align;
            }
        }

        size_t consumed = 0;
        size_t samples = 0;
        FLACDecoderResult result = output_32bit
                                       ? decoder.decode(payload.data() + offset, chunk_size,
                                                        out_ptr32, out_capacity, consumed, samples)
                                       : decoder.decode(payload.data() + offset, chunk_size,
                                                        out_ptr8, out_capacity, consumed, samples);

        // ---- Tier 1 oracle: structural invariants on a single decode -----------
        // The decoder must never report consuming more than it was handed.
        if (consumed > chunk_size) {
            std::abort();
        }
        if (decoder.get_stream_info().is_valid()) {
            const FLACStreamInfo& info = decoder.get_stream_info();
            const uint32_t channels = info.num_channels();
            const uint32_t bps = info.bits_per_sample();
            const uint32_t bytes_ps = info.bytes_per_sample();
            // FLAC bounds: 1..8 channels, 1..32 bits/sample (=> 1..4 bytes), and a
            // positive sample rate (is_valid() already implies the last). These bounds
            // currently follow unconditionally from num_channels()/bits_per_sample()'s
            // fixed-width bitfield decoding (cppcheck flags them as always-true), but the
            // check stays as a regression guard: it is the oracle that would catch a
            // future accessor bug widening or corrupting those fields.
            // cppcheck-suppress knownConditionTrueFalse
            if (channels == 0 || channels > 8) {
                std::abort();
            }
            // cppcheck-suppress knownConditionTrueFalse
            if (bps == 0 || bps > 32 || bytes_ps == 0 || bytes_ps > 4) {
                std::abort();
            }
            // Output is interleaved across every channel, so a frame contributes
            // block_size * channels samples => the total is a multiple of channels.
            if (samples > 0 && (samples % channels) != 0) {
                std::abort();
            }
            // A frame never exceeds max_block_size * channels samples, the bound the
            // caller uses to size the output buffer.
            if (samples > decoder.get_output_buffer_size_samples()) {
                std::abort();
            }
        }
        // Core memory-safety bound: a single decode() must never report more output
        // than the buffer holds. Native counts bytes (samples * bytes_per_sample),
        // 32-bit counts samples directly. Checked against the capacity we passed,
        // independent of stream_info, so a corrupt `samples` can't slip past.
        if (header_ready) {
            if (output_32bit) {
                if (samples > out_capacity) {
                    std::abort();
                }
            } else {
                const uint32_t bytes_ps = decoder.get_stream_info().bytes_per_sample();
                if (bytes_ps == 0 || samples > out_capacity / bytes_ps) {
                    std::abort();
                }
            }
        }
        // SUCCESS must mean at least one sample was produced.
        if (result == micro_flac::FLAC_DECODER_SUCCESS && samples == 0) {
            std::abort();
        }

        if (result == micro_flac::FLAC_DECODER_END_OF_STREAM) {
            break;
        }

        if (result < 0) {
            // Before HEADER_READY a negative result is fatal and latched (container
            // detection / STREAMINFO parse failure): stop. After HEADER_READY the
            // decoder resets frame state and the next call parses a fresh frame from
            // the current position, so advance past the bad bytes and continue into
            // the resync path. max(consumed, 1) guarantees forward progress.
            if (!header_ready) {
                break;
            }
            offset += consumed > 0 ? consumed : 1;
            continue;
        }

        if (result == micro_flac::FLAC_DECODER_HEADER_READY) {
            // Size the output buffer for one full frame now that STREAMINFO is known.
            // get_output_buffer_size_samples() == max_block_size * num_channels.
            size_t out_samples = decoder.get_output_buffer_size_samples();
            if (out_samples == 0 || out_samples > MAX_OUTPUT_SAMPLES) {
                break;  // degenerate / oversized header; nothing safe to decode
            }
            if (output_32bit) {
                out32.assign(out_samples, 0);
            } else {
                const uint32_t bytes_ps = decoder.get_stream_info().bytes_per_sample();
                // Exact size + out_align headroom so an over-write past the frame
                // lands in ASan's red zone rather than the alignment slack.
                out8.assign(out_samples * bytes_ps + out_align, 0);
            }
            header_ready = true;
        }

        offset += consumed;

        // Forward-progress guarantee: if nothing was consumed and nothing produced,
        // bail out instead of spinning.
        if (consumed == 0 && samples == 0) {
            break;
        }
    }
}

// NOLINTNEXTLINE(readability-identifier-naming): fixed libFuzzer entry point name
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzedDataProvider fdp(data, size);

    // Decoder configuration, read from the tail of the input. FuzzedDataProvider
    // takes integrals off the back, so the payload prefix stays intact and only the
    // trailing config/control bytes are peeled. A run peels up to ~72 bytes (1 cfg
    // byte, 7 metadata size-limit bytes when bit 2 is set, and up to 64 chunk-control
    // bytes); generate_seeds.sh pads each seed with a matching tail so the whole
    // stream survives. An exhausted provider reads 0: native output, CRC off, no
    // metadata, single pass, aligned.
    //
    // cfg layout:
    //   bit 0      enable CRC validation (default off reaches deeper frame decoding
    //              on mutated input: a CRC mismatch no longer aborts the frame)
    //   bit 1      use the 32-bit int32_t* output overload (else native uint8_t*)
    //   bit 2      store metadata blocks; also reads one size-limit byte per
    //              configurable block type from the tail (see ctrl_to_meta_size).
    //              Clear skips metadata and reads no per-type bytes
    //   bit 3      replay the whole stream a second time across a reset()
    //   bits 4-5   native output-buffer base alignment offset (0..3 bytes)
    const uint8_t cfg = fdp.ConsumeIntegral<uint8_t>();
    const bool enable_crc = (cfg & 0x01) != 0;
    const bool output_32bit = (cfg & 0x02) != 0;
    const bool store_metadata = (cfg & 0x04) != 0;
    const bool replay = (cfg & 0x08) != 0;
    const uint8_t out_align = (cfg >> 4) & 0x03;

    // Per-type metadata size limits, pulled from the tail only when bit 2 is set so
    // other inputs keep their payload bytes. One byte per configurable block type:
    // PADDING(1)..PICTURE(6) and the unknown-type bucket, in that order.
    uint32_t meta_sizes[MAX_META_BYTES] = {};
    if (store_metadata) {
        for (uint32_t& m : meta_sizes) {
            m = ctrl_to_meta_size(fdp.ConsumeIntegral<uint8_t>());
        }
    }

    // Reserve ~1/8 of the input (capped) for chunk-size control. If the input is
    // tiny, fall back to a single neutral control byte so the decoder still sees
    // the full payload at a moderate chunk size.
    const size_t ctrl_len = std::min(MAX_CONTROL_BYTES, fdp.remaining_bytes() / 8);
    std::vector<uint8_t> ctrl;
    ctrl.reserve(ctrl_len + 1);
    for (size_t i = 0; i < ctrl_len; i++) {
        ctrl.push_back(fdp.ConsumeIntegral<uint8_t>());
    }
    if (ctrl.empty()) {
        ctrl.push_back(0x20);  // neutral default: ~1 KiB chunks
    }

    const std::vector<uint8_t> payload = fdp.ConsumeRemainingBytes<uint8_t>();
    if (payload.empty()) {
        return 0;
    }

    FLACDecoder decoder;
    decoder.set_crc_check_enabled(enable_crc);
    if (store_metadata) {
        // Apply the per-type limits. Slot i targets block type i+1: PADDING(1)
        // through PICTURE(6), then any type >= 7 (the unknown bucket).
        for (size_t i = 0; i < MAX_META_BYTES; i++) {
            decoder.set_max_metadata_size(static_cast<FLACMetadataType>(i + 1), meta_sizes[i]);
        }
    }

    std::vector<uint8_t> out8;
    std::vector<int32_t> out32;

    run_decode_pass(decoder, payload, ctrl, output_32bit, out_align, out8, out32);

    if (replay) {
        // Replay the same payload across a reset() to drive the re-stream path. CRC
        // and metadata configuration are preserved across reset(); the block buffer
        // and any Ogg demuxer are freed and re-allocated.
        decoder.reset();
        run_decode_pass(decoder, payload, ctrl, output_32bit, out_align, out8, out32);
    }

    decoder.reset();
    return 0;
}

#ifdef FUZZ_STANDALONE

namespace {

std::vector<uint8_t> read_file(const char* path) {
    std::vector<uint8_t> out;
    FILE* f = std::fopen(path, "rb");
    if (!f)
        return out;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize(static_cast<size_t>(n));
        size_t got = std::fread(out.data(), 1, out.size(), f);
        out.resize(got);
    }
    std::fclose(f);
    return out;
}

uint32_t lcg_next(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

// Append the harness config tail to a raw FLAC payload so the whole payload
// survives the FuzzedDataProvider tail reads (which otherwise peel bytes off the
// back). Layout, by consumption order off the back: the cfg byte last, preceded
// by MAX_CONTROL_BYTES neutral chunk-control bytes.
std::vector<uint8_t> with_config_tail(std::vector<uint8_t> payload, uint8_t cfg) {
    payload.insert(payload.end(), MAX_CONTROL_BYTES, 0x20);  // ~1 KiB chunks
    payload.push_back(cfg);
    return payload;
}

// Build "fLaC" + a single last-block STREAMINFO metadata block with the given
// fields. Reaches HEADER_READY (no audio frames follow), exercising container
// detection and header parsing across the supported parameter ranges.
std::vector<uint8_t> make_flac_header(uint32_t sample_rate, uint8_t channels, uint8_t bits,
                                      uint16_t min_block, uint16_t max_block) {
    std::vector<uint8_t> v = {'f', 'L', 'a', 'C'};
    // Metadata block header: last-block flag (0x80) | type 0 (STREAMINFO), length 34.
    v.push_back(0x80);
    v.push_back(0);
    v.push_back(0);
    v.push_back(34);
    // 34-byte STREAMINFO, big-endian (see FLACStreamInfo raw layout).
    uint8_t s[34] = {};
    s[0] = static_cast<uint8_t>(min_block >> 8);
    s[1] = static_cast<uint8_t>(min_block & 0xFF);
    s[2] = static_cast<uint8_t>(max_block >> 8);
    s[3] = static_cast<uint8_t>(max_block & 0xFF);
    // s[4..9] min/max frame size left 0 (unknown).
    // s[10..12]: sample_rate (20 bits) | channels-1 (3 bits) | (bits-1) high bit.
    s[10] = static_cast<uint8_t>((sample_rate >> 12) & 0xFF);
    s[11] = static_cast<uint8_t>((sample_rate >> 4) & 0xFF);
    const uint8_t ch_field = static_cast<uint8_t>((channels - 1) & 0x07);
    const uint8_t bps_field = static_cast<uint8_t>((bits - 1) & 0x1F);
    s[12] = static_cast<uint8_t>(((sample_rate & 0x0F) << 4) | (ch_field << 1) | (bps_field >> 4));
    s[13] = static_cast<uint8_t>((bps_field & 0x0F)
                                 << 4);  // low 4 bps bits | total_samples high nibble (0)
    // s[13..17] total_samples remaining bytes 0 (unknown). s[18..33] MD5 0.
    v.insert(v.end(), s, s + 34);
    return v;
}

void mutate_in_place(std::vector<uint8_t>& buf, uint32_t& rng_state) {
    if (buf.empty())
        return;
    int n = 1 + static_cast<int>((lcg_next(rng_state) >> 24) & 0x07);
    for (int i = 0; i < n; i++) {
        uint32_t r = lcg_next(rng_state);
        size_t pos = r % buf.size();
        uint32_t kind = (r >> 24) & 0x07;
        switch (kind) {
            case 0:
            case 1:
                buf[pos] ^= static_cast<uint8_t>(1u << ((r >> 8) & 0x07));
                break;
            case 2:
            case 3:
                buf[pos] = static_cast<uint8_t>(r >> 16);
                break;
            case 4: {
                static const uint8_t interesting[] = {0x00, 0x01, 0x7F, 0x80,
                                                      0xFF, 0xFE, 0x55, 0xAA};
                buf[pos] = interesting[(r >> 16) & 0x07];
                break;
            }
            case 5: {
                size_t run = 1 + ((r >> 16) & 0x0F);
                for (size_t k = 0; k < run && pos + k < buf.size(); k++) {
                    buf[pos + k] = 0;
                }
                break;
            }
            case 6:
                buf[pos] = static_cast<uint8_t>(buf[pos] + 1);
                break;
            default:
                buf[pos] = static_cast<uint8_t>(buf[pos] - 1);
                break;
        }
    }
}

std::vector<uint8_t> build_random_blob(uint32_t seed, size_t len) {
    std::vector<uint8_t> buf(len);
    uint32_t state = seed;
    for (size_t i = 0; i < len; i++) {
        buf[i] = static_cast<uint8_t>(lcg_next(state) >> 24);
    }
    // Plant a "fLaC" magic at the front so the native parser engages on otherwise
    // random data.
    if (buf.size() >= 4) {
        const uint8_t magic[] = {'f', 'L', 'a', 'C'};
        std::memcpy(buf.data(), magic, sizeof(magic));
    }
    // Sprinkle FLAC frame sync codes (0xFF 0xF8) and OggS captures through the body
    // so frame-sync scanning and the Ogg demuxer get exercised.
    for (size_t i = 4; i + 4 < buf.size(); i += 64 + (seed % 192)) {
        if ((i & 1) == 0) {
            buf[i] = 0xFF;
            buf[i + 1] = 0xF8;
        } else {
            std::memcpy(buf.data() + i, "OggS", 4);
        }
    }
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    // Mutation mode: "./fuzz_flac_decode -mutate <seedfile>"
    if (argc >= 3 && std::strcmp(argv[1], "-mutate") == 0) {
        std::vector<uint8_t> seed = read_file(argv[2]);
        if (seed.empty()) {
            std::fprintf(stderr, "[fuzz] seed file %s is empty or missing\n", argv[2]);
            return 1;
        }
        const char* iter_env = std::getenv("FUZZ_ITERATIONS");
        const int iters = iter_env ? std::atoi(iter_env) : 2000;
        std::printf("[fuzz] mutation mode: seed=%s (%zu bytes), %d iterations\n", argv[2],
                    seed.size(), iters);

        uint32_t rng_state = 0xC0FFEEu;
        std::vector<uint8_t> scratch;
        scratch.reserve(seed.size());

        LLVMFuzzerTestOneInput(seed.data(), seed.size());

        for (int i = 0; i < iters; i++) {
            scratch = seed;
            mutate_in_place(scratch, rng_state);
            LLVMFuzzerTestOneInput(scratch.data(), scratch.size());
            if ((i + 1) % 200 == 0) {
                std::printf("[fuzz] %d/%d mutated iterations ok\n", i + 1, iters);
            }
        }
        std::printf("[fuzz] mutation fuzzing complete, no sanitizer failures\n");
        return 0;
    }

    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            std::vector<uint8_t> data = read_file(argv[i]);
            std::printf("[fuzz] %s (%zu bytes)\n", argv[i], data.size());
            LLVMFuzzerTestOneInput(data.data(), data.size());
        }
        std::printf("[fuzz] %d file(s) processed cleanly\n", argc - 1);
        return 0;
    }

    std::printf("[fuzz] standalone torture mode\n");

    // Empty / tiny inputs.
    {
        const uint8_t nothing[1] = {0};
        LLVMFuzzerTestOneInput(nothing, 0);
        LLVMFuzzerTestOneInput(nothing, 1);
    }

    // Bare / truncated container captures.
    {
        const uint8_t flac[4] = {'f', 'L', 'a', 'C'};
        LLVMFuzzerTestOneInput(flac, sizeof(flac));
        const uint8_t oggs[5] = {'O', 'g', 'g', 'S', 0};
        LLVMFuzzerTestOneInput(oggs, sizeof(oggs));
    }

    // A STREAMINFO claiming a huge metadata block length with no data behind it.
    {
        std::vector<uint8_t> v = {'f', 'L', 'a', 'C', 0x80, 0xFF, 0xFF, 0xFF};
        std::vector<uint8_t> in = with_config_tail(v, 0);
        LLVMFuzzerTestOneInput(in.data(), in.size());
    }

    // Valid headers across the supported parameter ranges, each under several cfg
    // combinations (native/32-bit output, CRC on/off, metadata, replay, alignment).
    {
        struct Variant {
            uint32_t rate;
            uint8_t channels;
            uint8_t bits;
            uint16_t min_block;
            uint16_t max_block;
        };
        static const Variant variants[] = {
            {44100, 2, 16, 4096, 4096}, {48000, 1, 24, 4096, 4096},  {96000, 2, 32, 1024, 4096},
            {8000, 1, 8, 16, 16},       {192000, 8, 16, 4608, 4608}, {22050, 6, 20, 512, 512},
        };
        for (const Variant& v : variants) {
            std::vector<uint8_t> hdr =
                make_flac_header(v.rate, v.channels, v.bits, v.min_block, v.max_block);
            for (uint8_t cfg : {uint8_t{0x00}, uint8_t{0x01}, uint8_t{0x02}, uint8_t{0x04},
                                uint8_t{0x08}, uint8_t{0x12}, uint8_t{0x3F}}) {
                std::vector<uint8_t> in = with_config_tail(hdr, cfg);
                LLVMFuzzerTestOneInput(in.data(), in.size());
            }
        }
    }

    // Direct decode() calls covering a guard path the streaming loop avoids: a
    // zero-size output buffer after HEADER_READY (the loop always supplies a
    // full-frame buffer). The null pointers below are *output* buffers passed
    // during header parsing, where output is legitimately unused; the input
    // pointer is always a valid libFuzzer payload.
    {
        std::vector<uint8_t> hdr = make_flac_header(44100, 2, 16, 4096, 4096);
        FLACDecoder decoder;
        size_t consumed = 0;
        size_t samples = 0;
        uint8_t one_byte = 0;

        // Parse the header fully (output unused during header parsing).
        FLACDecoderResult r = decoder.decode(hdr.data(), hdr.size(), static_cast<uint8_t*>(nullptr),
                                             0, consumed, samples);
        size_t off = consumed;
        while (r == micro_flac::FLAC_DECODER_NEED_MORE_DATA && off < hdr.size()) {
            size_t ate = 0;
            r = decoder.decode(hdr.data() + off, hdr.size() - off, static_cast<uint8_t*>(nullptr),
                               0, ate, samples);
            off += ate;
        }
        // Zero-size output buffer for a frame decode (no frames follow, but the
        // guard still runs). Result intentionally unchecked: this call only exists to
        // exercise the zero-capacity guard path, not to assert a particular outcome.
        (void)decoder.decode(hdr.data() + off, hdr.size() - off, &one_byte, 0, consumed, samples);
    }

    // Random blobs with planted fLaC/OggS magics and frame sync codes.
    const char* iter_env = std::getenv("FUZZ_ITERATIONS");
    const int kIterations = iter_env ? std::atoi(iter_env) : 200;
    for (int i = 0; i < kIterations; i++) {
        size_t len = 64 + (static_cast<size_t>(i) * 37) % (32 * 1024);
        std::vector<uint8_t> blob = build_random_blob(static_cast<uint32_t>(i) * 2654435761u, len);
        LLVMFuzzerTestOneInput(blob.data(), blob.size());
        if ((i + 1) % 200 == 0) {
            std::printf("[fuzz] %d/%d random iterations ok\n", i + 1, kIterations);
        }
    }

    std::printf("[fuzz] standalone torture complete, no sanitizer failures\n");
    return 0;
}

#endif  // FUZZ_STANDALONE
