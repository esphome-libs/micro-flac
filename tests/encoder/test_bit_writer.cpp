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

// Assert-based unit test for src/bit_writer.h, checked against bit_reader.h.

// assert() is the whole test, so it must survive a Release build (whose
// NDEBUG would otherwise compile every check out and let the test pass
// without checking anything).
#undef NDEBUG

#include "bit_reader.h"
#include "bit_writer.h"
#include "compiler.h"  // BIT_BUFFER_BITS

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace micro_flac;

// This test file is essentially wall-to-wall literal test vectors (widths,
// residuals, Rice parameters, expected byte patterns); naming each one would
// only obscure what value is actually being exercised. Suppressed for the
// whole file rather than per line.
// NOLINTBEGIN(readability-magic-numbers)

namespace {

// ============================================================================
// Reference bit packer (test oracle)
// ============================================================================
//
// Deliberately simple/unoptimized: appends one bit at a time into a
// std::vector<bool>, independent of BitWriterLocal's chunked-accumulator
// implementation. Used as ground truth for exact-byte comparisons.
class RefBits {
public:
    /// @brief Append the low `nbits` bits of `value`, MSB-first
    ///
    /// `nbits` may exceed 32 when the test uses this to represent a wider
    /// conceptual field that is zero-extended above bit 31 (see
    /// test_write_uint_split_chunk_forced()); guard the shift so that stays
    /// well-defined instead of shifting a uint32_t by >= 32.
    ///
    /// @param value Bits to append (low `nbits` used)
    /// @param nbits Number of bits to append
    void append_uint(uint32_t value, uint8_t nbits) {
        for (int i = static_cast<int>(nbits) - 1; i >= 0; --i) {
            const bool bit = (i < 32) && (((value >> i) & 1U) != 0);
            this->bits_.push_back(bit);
        }
    }

    /// @brief Append one Rice code: zigzag + unary + stop bit + low k bits
    ///
    /// Per RFC 9639 SS9.2.7. Computed independently of write_rice()'s
    /// `(r << 1) ^ (r >> 31)` formulation.
    ///
    /// @param residual Signed residual to encode
    /// @param k Rice parameter (0-30)
    void append_rice(int32_t residual, uint8_t k) {
        uint32_t u = 0;
        if (residual >= 0) {
            u = static_cast<uint32_t>(residual) * 2U;
        } else {
            u = static_cast<uint32_t>(-(residual + 1)) * 2U + 1U;
        }
        const uint32_t quotient = u >> k;
        for (uint32_t i = 0; i < quotient; ++i) {
            this->bits_.push_back(false);
        }
        this->bits_.push_back(true);
        if (k > 0) {
            const uint32_t mask = (k >= 32) ? 0xFFFFFFFFU : ((1U << k) - 1U);
            this->append_uint(u & mask, k);
        }
    }

    /// @brief Append zero bits up to the next byte boundary
    void append_align() {
        while (this->bits_.size() % 8 != 0) {
            this->bits_.push_back(false);
        }
    }

    /// @brief Pack the appended bits into bytes, MSB-first
    /// @return Packed bytes (final partial byte zero-padded)
    std::vector<uint8_t> pack() const {
        std::vector<uint8_t> out((this->bits_.size() + 7) / 8, 0);
        for (size_t i = 0; i < this->bits_.size(); ++i) {
            if (this->bits_[i]) {
                out[i / 8] |= static_cast<uint8_t>(1U << (7 - (i % 8)));
            }
        }
        return out;
    }

private:
    // Struct fields
    std::vector<bool> bits_;
};

// ============================================================================
// write_uint: every width 1-32, known patterns
// ============================================================================

void test_write_uint_widths() {
    for (uint32_t w = 1; w <= 32; ++w) {
        const uint8_t width = static_cast<uint8_t>(w);
        const uint32_t mask = (w >= 32) ? 0xFFFFFFFFU : ((1U << w) - 1U);
        const uint32_t patterns[] = {0U, mask, 0xA5A5A5A5U & mask, 0x12345678U & mask, 1U & mask};

        for (uint32_t value : patterns) {
            RefBits ref;
            ref.append_uint(value, width);
            const std::vector<uint8_t> expected = ref.pack();

            uint8_t buf[8] = {};
            BitWriterLocal bw{};
            writer_init(bw, buf, sizeof(buf));
            write_uint(bw, value, width);
            assert(!bw.overflow);

            const size_t total = writer_flush(bw, buf);
            assert(total == expected.size());
            assert(memcmp(buf, expected.data(), expected.size()) == 0);
        }
    }
    std::printf("  write_uint widths 1-32: OK\n");
}

// White-box test: force the multi-chunk split path inside write_uint() by
// directly seeding BitWriterLocal's pending-bit state, regardless of the
// host's BIT_BUFFER_BITS. On a 64-bit host, a single <=32-bit write never
// naturally needs to split (nbits is always < 8 going in, so 7 + 32 < 64);
// only the 32-bit (Xtensa) accumulator hits that path via normal calls. This
// exercises the same code deterministically on any host.
void test_write_uint_split_chunk_forced() {
    uint8_t buf[32] = {};
    BitWriterLocal bw{};
    writer_init(bw, buf, sizeof(buf));

    const uint32_t pending_bits = (BIT_BUFFER_BITS == 64) ? 40U : 7U;
    const uint32_t pending_pattern = 0x5U;  // arbitrary, fits comfortably in pending_bits
    bw.acc = static_cast<bit_writer_acc_t>(pending_pattern) << (BIT_BUFFER_BITS - pending_bits);
    bw.nbits = pending_bits;

    const uint32_t value = 0xA5A5A5A5U;
    write_uint(bw, value, 32);
    assert(!bw.overflow);
    write_align_zero(bw);

    RefBits ref;
    ref.append_uint(pending_pattern, static_cast<uint8_t>(pending_bits));
    ref.append_uint(value, 32);
    ref.append_align();
    const std::vector<uint8_t> expected = ref.pack();

    const size_t total = writer_flush(bw, buf);
    assert(total == expected.size());
    assert(memcmp(buf, expected.data(), expected.size()) == 0);
    std::printf("  write_uint forced multi-chunk split (pending_bits=%u): OK\n", pending_bits);
}

// ============================================================================
// Rice codes: k = 0..30, positive/negative/zero/extreme residuals
// ============================================================================

void test_rice_codes() {
    static const int32_t RESIDUALS[] = {
        0,
        1,
        -1,
        2,
        -2,
        100,
        -100,
        // INT16_MIN/MAX
        -32768,
        32767,
        // 17-bit side-channel extremes
        65535,
        -65535,
        65536,
        -65536,
        131071,
        -131071,
        // Order-4 fixed-predictor worst case for a 17-bit side signal (~16 * 2^17)
        2097151,
        -2097151,
        2097152,
        -2097152,
        // 25-bit side channel of a 24-bit stream, its order-4 fixed worst
        // case, and the int32 extremes: parameters above 14 (5-bit
        // parameters, coding method 01) exist for these
        16777215,
        -16777216,
        268435455,
        -268435456,
        INT32_MAX,
        INT32_MIN,
    };

    uint32_t checked = 0;
    for (uint32_t k32 = 0; k32 <= 30; ++k32) {
        const uint8_t k = static_cast<uint8_t>(k32);
        for (int32_t r : RESIDUALS) {
            // Skip pairings whose unary run alone would be megabytes long
            // (a huge residual at a small parameter): the checked general
            // path they take is covered at smaller scale below.
            // cppcheck-suppress shiftTooManyBitsSigned
            const uint32_t u = (static_cast<uint32_t>(r) << 1) ^ static_cast<uint32_t>(r >> 31);
            if ((u >> k) > (1U << 20)) {
                continue;
            }
            checked++;
            RefBits ref;
            ref.append_rice(r, k);
            const std::vector<uint8_t> expected = ref.pack();

            std::vector<uint8_t> buf(expected.size() + 64, 0);
            BitWriterLocal bw{};
            writer_init(bw, buf.data(), buf.size());
            write_rice(bw, r, k);
            assert(!bw.overflow);

            const size_t total = writer_flush(bw, buf.data());
            assert(total == expected.size());
            assert(memcmp(buf.data(), expected.data(), expected.size()) == 0);
        }
    }
    std::printf("  rice codes k=0..30 x %zu residuals (%u pairings): OK\n",
                sizeof(RESIDUALS) / sizeof(RESIDUALS[0]), checked);
}

// ============================================================================
// Unaligned mixed sequences + alignment behavior
// ============================================================================

void test_unaligned_mixed_sequences() {
    uint8_t buf[64] = {};
    BitWriterLocal bw{};
    writer_init(bw, buf, sizeof(buf));

    RefBits ref;

    // A 3-bit field (e.g. subframe header padding+type-ish width), a 17-bit
    // field (side-channel warmup sample width), a couple of Rice codes, an
    // explicit alignment, an 8-bit field, another Rice code, and a final
    // alignment -- representative of a real subframe's bit sequence.
    write_uint(bw, 0x5U, 3);
    ref.append_uint(0x5U, 3);

    write_uint(bw, 0x1FFFFU, 17);
    ref.append_uint(0x1FFFFU, 17);

    write_rice(bw, -12345, 11);
    ref.append_rice(-12345, 11);

    write_rice(bw, 0, 0);
    ref.append_rice(0, 0);

    write_align_zero(bw);
    ref.append_align();

    write_uint(bw, 0xABU, 8);
    ref.append_uint(0xABU, 8);

    // k chosen proportional to magnitude (as a real encoder would) so the
    // unary quotient stays small; test_rice_codes() already covers
    // deliberately mismatched (small k, large residual) pairings.
    write_rice(bw, 65535, 16);
    ref.append_rice(65535, 16);

    write_rice(bw, -70000, 17);
    ref.append_rice(-70000, 17);

    write_align_zero(bw);
    ref.append_align();

    assert(!bw.overflow);
    const std::vector<uint8_t> expected = ref.pack();
    const size_t total = writer_flush(bw, buf);
    assert(total == expected.size());
    assert(memcmp(buf, expected.data(), expected.size()) == 0);
    std::printf("  unaligned mixed sequence: OK (%zu bytes)\n", total);
}

// ============================================================================
// writer_flush byte counts: aligned and unaligned tails
// ============================================================================

void test_flush_byte_counts() {
    // Aligned tail: 3 whole bytes, already aligned before flush (fast path).
    {
        uint8_t buf[8] = {};
        BitWriterLocal bw{};
        writer_init(bw, buf, sizeof(buf));
        write_uint(bw, 0x11U, 8);
        write_uint(bw, 0x22U, 8);
        write_uint(bw, 0x33U, 8);
        assert(bw.nbits == 0);  // already byte-aligned going into flush
        const size_t total = writer_flush(bw, buf);
        assert(total == 3);
        assert(buf[0] == 0x11 && buf[1] == 0x22 && buf[2] == 0x33);
    }

    // Unaligned tail: 13 bits (5 + 8) needs 2 bytes, last one zero-padded.
    {
        uint8_t buf[8] = {};
        BitWriterLocal bw{};
        writer_init(bw, buf, sizeof(buf));
        write_uint(bw, 0x15U, 5);  // 10101
        write_uint(bw, 0xCDU, 8);  // 11001101
        assert(bw.nbits == 5);     // 13 bits total = 1 byte + 5 leftover bits
        const size_t total = writer_flush(bw, buf);
        assert(total == 2);

        RefBits ref;
        ref.append_uint(0x15U, 5);
        ref.append_uint(0xCDU, 8);
        const std::vector<uint8_t> expected = ref.pack();
        assert(memcmp(buf, expected.data(), expected.size()) == 0);
    }

    // Flushing with nothing written at all: 0 bytes, no overflow.
    {
        uint8_t buf[8] = {};
        BitWriterLocal bw{};
        writer_init(bw, buf, sizeof(buf));
        const size_t total = writer_flush(bw, buf);
        assert(total == 0);
        assert(!bw.overflow);
    }

    std::printf("  writer_flush byte counts (aligned/unaligned/empty): OK\n");
}

// ============================================================================
// Overflow behavior
// ============================================================================

void test_overflow_behavior() {
    // Exactly-full buffer: fits with zero bits to spare -> OK, no overflow.
    {
        uint8_t buf[2] = {};
        BitWriterLocal bw{};
        writer_init(bw, buf, sizeof(buf));
        write_uint(bw, 0xBEEFU, 16);
        assert(!bw.overflow);
        const size_t total = writer_flush(bw, buf);
        assert(total == 2);
        assert(buf[0] == 0xBE && buf[1] == 0xEF);
    }

    // A write that needs a 3rd full byte from a 2-byte buffer overflows
    // immediately inside write_uint() itself (flush_full_bytes() hits the
    // `pos >= end` check while draining the 3rd byte).
    {
        uint8_t buf[2] = {};
        BitWriterLocal bw{};
        writer_init(bw, buf, sizeof(buf));
        write_uint(bw, 0xFFFFFFU, 24);
        assert(bw.overflow);
        const size_t total = writer_flush(bw, buf);
        assert(total == 0);
    }

    // One bit too many: a 17-bit write into a 2-byte (16-bit) buffer stores
    // its 2 complete bytes immediately (that does not write past `end`) and
    // leaves the 17th bit pending in the accumulator -- overflow is only
    // detected once something tries to flush that pending bit out as a 3rd
    // byte, i.e. at writer_flush() time.
    {
        uint8_t buf[2] = {};
        BitWriterLocal bw{};
        writer_init(bw, buf, sizeof(buf));
        write_uint(bw, 0x1FFFFU, 17);
        assert(!bw.overflow);  // 2 bytes flushed, 1 bit still pending in acc
        const size_t total = writer_flush(bw, buf);
        assert(bw.overflow);  // padding the pending bit needs a 3rd byte
        assert(total == 0);
    }

    // Canary byte immediately after the usable buffer must never be
    // touched, even when a write massively overflows.
    {
        uint8_t buf[5];  // [0..2] usable (capacity=3), [3] canary, [4] guard
        memset(buf, 0, sizeof(buf));
        buf[3] = 0xCC;
        buf[4] = 0xCC;
        BitWriterLocal bw{};
        writer_init(bw, buf, 3);

        write_uint(bw, 0xFFFFFFFFU, 32);  // way more than 3 bytes' worth
        assert(bw.overflow);
        assert(buf[3] == 0xCC);
        assert(buf[4] == 0xCC);

        // Further writes after overflow must remain no-ops.
        write_uint(bw, 0x7U, 3);
        write_rice(bw, 12345, 2);
        write_zeros(bw, 1000);
        write_align_zero(bw);
        assert(bw.overflow);
        assert(buf[3] == 0xCC);
        assert(buf[4] == 0xCC);
        assert(bw.pos <= bw.end);

        const size_t total = writer_flush(bw, buf);
        assert(total == 0);
    }

    // write_zeros() overflow: a huge zero run into a tiny buffer must fill
    // only up to `end` (via the memset fast path) and never write past it.
    {
        uint8_t buf[3];  // [0..1] usable (capacity=2), [2] canary
        memset(buf, 0, sizeof(buf));
        buf[2] = 0xCC;
        BitWriterLocal bw{};
        writer_init(bw, buf, 2);

        write_zeros(bw, 1000000);
        assert(bw.overflow);
        assert(buf[0] == 0x00 && buf[1] == 0x00);  // filled with legitimate zero bytes
        assert(buf[2] == 0xCC);                    // canary untouched
        assert(bw.pos == bw.end);
    }

    // write_rice() overflow via a huge unary run (k=0, large residual) into
    // a tiny buffer.
    {
        uint8_t buf[3];
        memset(buf, 0, sizeof(buf));
        buf[2] = 0xCC;
        BitWriterLocal bw{};
        writer_init(bw, buf, 2);

        write_rice(bw, 2097152, 0);
        assert(bw.overflow);
        assert(buf[2] == 0xCC);
    }

    std::printf("  overflow behavior (exact-fit/one-bit-over/canary/write_zeros/write_rice): OK\n");
}

// ============================================================================
// Round-trip through src/bit_reader.h primitives
// ============================================================================

void test_round_trip() {
    constexpr uint32_t NUM_OPS = 3000;
    constexpr int32_t RESIDUAL_RANGE = 3000;  // keeps worst-case unary runs bounded

    // Fixed seed: deterministic/reproducible test output, not a security context.
    // NOLINTNEXTLINE(bugprone-random-generator-seed)
    std::mt19937 rng(12345U);
    std::uniform_int_distribution<int> op_dist(0, 1);
    std::uniform_int_distribution<uint32_t> width_dist(1U, 32U);
    std::uniform_int_distribution<uint32_t> k_dist(0U, 30U);
    std::uniform_int_distribution<int32_t> residual_dist(-RESIDUAL_RANGE, RESIDUAL_RANGE);

    struct Op {
        bool is_rice;
        uint32_t value;
        uint8_t width;
        int32_t residual;
        uint8_t k;
    };
    std::vector<Op> ops;
    ops.reserve(NUM_OPS);

    // Multiply in size_t so the vector constructor never sees an
    // implicitly-widened 32-bit intermediate result.
    std::vector<uint8_t> buffer(static_cast<size_t>(8) * 1024 * 1024,
                                0);  // generous; see RESIDUAL_RANGE comment
    BitWriterLocal bw{};
    writer_init(bw, buffer.data(), buffer.size());

    for (uint32_t i = 0; i < NUM_OPS; ++i) {
        if (op_dist(rng) == 0) {
            const uint8_t width = static_cast<uint8_t>(width_dist(rng));
            const uint32_t max_val = (width >= 32) ? 0xFFFFFFFFU : ((1U << width) - 1U);
            std::uniform_int_distribution<uint32_t> val_dist(0U, max_val);
            const uint32_t value = val_dist(rng);
            write_uint(bw, value, width);
            ops.push_back(Op{false, value, width, 0, 0});
        } else {
            const uint8_t k = static_cast<uint8_t>(k_dist(rng));
            int32_t residual = residual_dist(rng);
            if (k > 14) {
                // Parameters above 14 come with residuals near 2^k; scale
                // into that range (at most a few quotient bits each).
                const uint32_t span_bits = (k + 2U < 30U) ? (k + 2U) : 30U;
                std::uniform_int_distribution<int32_t> wide_dist(-(1 << span_bits),
                                                                 (1 << span_bits) - 1);
                residual = wide_dist(rng);
            }
            write_rice(bw, residual, k);
            ops.push_back(Op{true, 0, 0, residual, k});
        }
        assert(!bw.overflow);
    }
    write_align_zero(bw);
    const size_t total = writer_flush(bw, buffer.data());
    assert(!bw.overflow);
    assert(total > 0);

    BitReaderLocal br{};
    br.bit_buffer = 0;
    br.buffer = buffer.data();
    br.buffer_index = 0;
    br.bytes_left = total;
    br.bit_buffer_length = 0;

    for (const Op& op : ops) {
        bool out_of_data = false;
        if (!op.is_rice) {
            const uint32_t value = read_uint_local(br, op.width, out_of_data);
            assert(!out_of_data);
            assert(value == op.value);
        } else {
            const uint32_t mask = (op.k >= 32) ? 0xFFFFFFFFU : ((1U << op.k) - 1U);
            uint32_t unary_out = 0;
            bool binary_pending_out = false;
            const int32_t value =
                read_rice_sint_local(br, op.k, mask, &out_of_data, &unary_out, &binary_pending_out);
            assert(!out_of_data);
            assert(value == op.residual);
        }
    }

    std::printf("  round-trip through bit_reader.h (%u ops, %zu bytes): OK\n", NUM_OPS, total);
}

// ============================================================================
// write_rice_block() vs. per-code write_rice() equivalence
// ============================================================================
//
// write_rice_block() promises bytes identical to calling write_rice() per
// residual, given the caller's capacity guarantee. Fuzz random residual
// blocks (mixed magnitudes, including huge values that force long unary
// runs through the checked general path, and every k in 0-30), at every
// starting bit misalignment, and compare buffers exactly.
void test_rice_block_equivalence() {
    std::mt19937 rng(0xB10CU);

    for (uint32_t iter = 0; iter < 2000; iter++) {
        const uint8_t k = static_cast<uint8_t>(rng() % 31);
        const uint32_t count = 1 + static_cast<uint32_t>(rng() % 300);
        const uint32_t misalign_bits = rng() % 8;  // starting nbits in [0, 8)

        std::vector<int32_t> residuals(count);
        for (uint32_t i = 0; i < count; i++) {
            switch (rng() % 5) {
                case 0:  // small, typical
                    residuals[i] = static_cast<int32_t>(rng() % 64) - 32;
                    break;
                case 1:  // medium
                    residuals[i] = static_cast<int32_t>(rng() % 65536) - 32768;
                    break;
                case 2:  // 17-bit-signal-scale extremes
                    residuals[i] = static_cast<int32_t>(rng() % (1U << 22)) - (1 << 21);
                    break;
                case 3: {  // up to 2^(k + 3): the scale parameters above 14 code
                    const uint32_t bits = (k + 3U < 31U) ? (k + 3U) : 31U;
                    residuals[i] = static_cast<int32_t>(rng()) >> (32U - bits);
                    break;
                }
                default:  // pathological: force long unary runs (q >> space)
                    residuals[i] = static_cast<int32_t>(rng() % (1U << 22)) - (1 << 21);
                    residuals[i] *= 8;  // widen past the fast path's headroom
                    break;
            }
        }

        // Exact capacity from the codes' true bit lengths (matching the
        // encoder's own capacity-proof arithmetic) plus slack.
        uint64_t total_bits = misalign_bits;
        for (uint32_t i = 0; i < count; i++) {
            const int32_t r = residuals[i];
            // cppcheck-suppress shiftTooManyBitsSigned
            const uint32_t u = (static_cast<uint32_t>(r) << 1) ^ static_cast<uint32_t>(r >> 31);
            total_bits += (u >> k) + 1U + k;
        }
        const size_t cap = static_cast<size_t>(total_bits / 8) + 16;
        static std::vector<uint8_t> buf_ref;
        static std::vector<uint8_t> buf_blk;
        buf_ref.assign(cap, 0xAA);
        buf_blk.assign(cap, 0xAA);

        BitWriterLocal ref{};
        writer_init(ref, buf_ref.data(), cap);
        BitWriterLocal blk{};
        writer_init(blk, buf_blk.data(), cap);
        if (misalign_bits > 0) {
            write_uint(ref, 0x5A5A5A5AU, static_cast<uint8_t>(misalign_bits));
            write_uint(blk, 0x5A5A5A5AU, static_cast<uint8_t>(misalign_bits));
        }

        for (uint32_t i = 0; i < count; i++) {
            write_rice(ref, residuals[i], k);
        }
        write_rice_block(blk, residuals.data(), count, k);

        assert(!ref.overflow && !blk.overflow);
        assert(ref.nbits == blk.nbits);
        assert(ref.acc == blk.acc);
        const size_t ref_bytes = writer_flush(ref, buf_ref.data());
        const size_t blk_bytes = writer_flush(blk, buf_blk.data());
        assert(ref_bytes == blk_bytes);
        assert(std::memcmp(buf_ref.data(), buf_blk.data(), ref_bytes) == 0);
    }

    std::printf("  write_rice_block == per-code write_rice (2000 fuzz blocks): OK\n");
}

}  // namespace

// NOLINTEND(readability-magic-numbers)

int main() {  // NOLINT(bugprone-exception-escape)
    test_write_uint_widths();
    test_write_uint_split_chunk_forced();
    test_rice_codes();
    test_unaligned_mixed_sequences();
    test_flush_byte_counts();
    test_overflow_behavior();
    test_round_trip();
    test_rice_block_equivalence();

    std::printf("test_bit_writer: all tests passed\n");
    return 0;
}
