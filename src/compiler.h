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

/// @file compiler.h
/// @brief Compiler hints and platform-specific macros
///
/// Provides optimization attributes, branch prediction hints, forced inlining,
/// and bit buffer constants.

#pragma once

// O3 for critical functions in GCC builds, whatever the project's level
#if defined(__GNUC__) && !defined(__clang__)
#define FLAC_OPTIMIZE_O3 __attribute__((optimize("O3")))
#else
#define FLAC_OPTIMIZE_O3
#endif

// Force inline for performance-critical functions
#if defined(__GNUC__) || defined(__clang__)
#define FLAC_ALWAYS_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define FLAC_ALWAYS_INLINE __forceinline
#else
#define FLAC_ALWAYS_INLINE inline
#endif

// Cache line alignment for hot path functions
// Prevents performance regressions from code layout changes in preceding functions
#if defined(__GNUC__) || defined(__clang__)
#define FLAC_HOT __attribute__((hot))
#else
#define FLAC_HOT
#endif

// Prevent inlining. Useful for extracting a tight loop into its own function
// so the compiler can allocate registers for it without pressure from a
// surrounding large switch/state machine.
#if defined(__GNUC__) || defined(__clang__)
#define FLAC_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define FLAC_NOINLINE __declspec(noinline)
#else
#define FLAC_NOINLINE
#endif

// Branch prediction hints
#if defined(__GNUC__) || defined(__clang__)
#define FLAC_LIKELY(x) __builtin_expect(!!(x), 1)
#define FLAC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define FLAC_LIKELY(x) (x)
#define FLAC_UNLIKELY(x) (x)
#endif

// Hint to the optimizer that an expression is true. Used to prune impossible
// branches and tighten codegen on the hot path. Clang has `__builtin_assume`;
// GCC lacks a direct equivalent, so fall back to the
// `if (!x) __builtin_unreachable();` idiom (which evaluates `x`, unlike
// __builtin_assume, so keep `x` side-effect free).
#if defined(__clang__)
#define FLAC_ASSUME(x) __builtin_assume(x)
#elif defined(__GNUC__)
#define FLAC_ASSUME(x)               \
    do {                             \
        if (!(x))                    \
            __builtin_unreachable(); \
    } while (0)
#else
#define FLAC_ASSUME(x) ((void)0)
#endif

// Marks a type as able to alias any other, like char
#if defined(__GNUC__) || defined(__clang__)
#define FLAC_MAY_ALIAS __attribute__((__may_alias__))
#else
#define FLAC_MAY_ALIAS
#endif

// 64-bit little-endian hosts with cheap unaligned 32-bit loads. Xtensa faults
// on them and RISC-V may trap, so embedded targets leave this undefined.
#if (defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64)) && \
    (!defined(__BYTE_ORDER__) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define FLAC_FAST_UNALIGNED_LOADS 1
#endif

// Silence UBSan signed-integer-overflow in LPC restore functions.
// FLAC LPC prediction intentionally uses wrapping int32_t arithmetic;
// overflows in this path are audio-only and not a security concern.
// Same approach as libFLAC (FLAC__lpc_restore_signal).
#if defined(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION) && (defined(__GNUC__) || defined(__clang__))
#define FLAC_NO_SANITIZE_OVERFLOW __attribute__((no_sanitize("signed-integer-overflow")))
#else
#define FLAC_NO_SANITIZE_OVERFLOW
#endif

#include <cstdint>

namespace micro_flac {

// Types for reading packed PCM bytes more than one byte at a time. The
// buffer's real type is the caller's, so these must be allowed to alias it.
typedef int16_t FLAC_MAY_ALIAS aliased_int16_t;
typedef uint32_t FLAC_MAY_ALIAS aliased_uint32_t;

// ============================================================================
// Bit Buffer Constants (implementation-only)
// ============================================================================

// bit_buffer_t (flac_decoder.h) and bit_writer.h's accumulator are this wide.
// Use a 32-bit bit buffer on 32-bit platforms (Xtensa, ARM32, etc.) and a 64-bit
// bit buffer on 64-bit platforms to reduce refill frequency.
#if UINTPTR_MAX == 0xFFFFFFFF
#define BIT_BUFFER_BITS 32
#define FLAC_CLZ(x) __builtin_clz(x)
#else
#define BIT_BUFFER_BITS 64
#define FLAC_CLZ(x) __builtin_clzll(x)
#endif

// Number of bytes that fill the bit buffer completely
#define BIT_BUFFER_BYTES (BIT_BUFFER_BITS / 8)

}  // namespace micro_flac
