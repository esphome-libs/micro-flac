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

/// @file alloc.h
/// @brief Memory allocation macros for FLAC decoder buffers
///
/// Provides FLAC_MALLOC and FLAC_FREE macros with ESP-IDF PSRAM support and
/// configurable memory placement. Can be overridden via compiler defines.

#pragma once

// Include necessary headers based on platform
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#else
#include <cstdlib>  // IWYU pragma: keep (malloc/free, used by the FLAC_MALLOC/FLAC_FREE expansions)
#endif

// ============================================================================
// Memory Allocation Customization
// ============================================================================

// Define memory allocation macros - can be overridden via compiler defines
// Example: -DFLAC_MALLOC=my_malloc -DFLAC_FREE=my_free
// Both FLAC_MALLOC and FLAC_FREE should be defined together as a pair.

#if defined(FLAC_MALLOC) != defined(FLAC_FREE)
#error "FLAC_MALLOC and FLAC_FREE must both be defined or both left undefined."
#endif

#ifndef FLAC_MALLOC
#ifdef ESP_PLATFORM
// Internal memory is requested as DRAM first. On the ESP32, MALLOC_CAP_INTERNAL |
// MALLOC_CAP_32BIT also matches IRAM, where data loads are about 3.7x slower than from DRAM
// (the 32-bit order-8 LPC loop: 103 cycles per sample from IRAM, 28 from DRAM, 36 from 40 MHz
// PSRAM). MALLOC_CAP_8BIT excludes IRAM, so IRAM is only a last resort.
#define FLAC_MALLOC_CAPS_DRAM (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define FLAC_MALLOC_CAPS_ANY_INTERNAL (MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT)
#define FLAC_MALLOC_CAPS_PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_32BIT)
#if defined(MICRO_FLAC_MEMORY_PREFER_PSRAM)
#define FLAC_MALLOC(sz)                                                           \
    heap_caps_malloc_prefer(sz, 3, FLAC_MALLOC_CAPS_PSRAM, FLAC_MALLOC_CAPS_DRAM, \
                            FLAC_MALLOC_CAPS_ANY_INTERNAL)
#elif defined(MICRO_FLAC_MEMORY_PREFER_INTERNAL)
#define FLAC_MALLOC(sz)                                                           \
    heap_caps_malloc_prefer(sz, 3, FLAC_MALLOC_CAPS_DRAM, FLAC_MALLOC_CAPS_PSRAM, \
                            FLAC_MALLOC_CAPS_ANY_INTERNAL)
#elif defined(MICRO_FLAC_MEMORY_PSRAM_ONLY)
#define FLAC_MALLOC(sz) heap_caps_malloc(sz, FLAC_MALLOC_CAPS_PSRAM)
#elif defined(MICRO_FLAC_MEMORY_INTERNAL_ONLY)
#define FLAC_MALLOC(sz) \
    heap_caps_malloc_prefer(sz, 2, FLAC_MALLOC_CAPS_DRAM, FLAC_MALLOC_CAPS_ANY_INTERNAL)
#else
// Default: prefer PSRAM, fall back to internal
#define FLAC_MALLOC(sz)                                                           \
    heap_caps_malloc_prefer(sz, 3, FLAC_MALLOC_CAPS_PSRAM, FLAC_MALLOC_CAPS_DRAM, \
                            FLAC_MALLOC_CAPS_ANY_INTERNAL)
#endif
#else
#define FLAC_MALLOC(sz) malloc(sz)
#endif
#endif

#ifndef FLAC_FREE
#ifdef ESP_PLATFORM
#define FLAC_FREE(p) heap_caps_free(p)
#else
#define FLAC_FREE(p) free(p)
#endif
#endif
