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

// LPC Assembly Correctness Test
//
// Compares Xtensa assembly LPC restoration output against reference C++ output
// captured from host decoding of real FLAC files.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "test_lpc_vectors.h"
#include "xtensa/lpc_xtensa.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void app_main(void) {
    // Delay to allow serial monitor to connect after upload
    vTaskDelay(pdMS_TO_TICKS(2000));

    printf("\n========================================\n");
    printf("   LPC Assembly Correctness Test\n");
    printf("========================================\n");
    printf("Total test vectors: %u\n\n", (unsigned)lpc_test_vector_count);

    uint32_t pass_count = 0;
    uint32_t fail_count = 0;

    for (size_t i = 0; i < lpc_test_vector_count; i++) {
        const LpcTestVector& vec = lpc_test_vectors[i];

        // Allocate and copy input buffer (assembly modifies in-place)
        int32_t* buffer = static_cast<int32_t*>(malloc(vec.num_samples * sizeof(int32_t)));
        if (!buffer) {
            printf("FAIL [%u]: malloc failed\n", (unsigned)i);
            fail_count++;
            continue;
        }
        memcpy(buffer, vec.input_buffer, vec.num_samples * sizeof(int32_t));

        // Call the appropriate assembly function
        if (vec.is_64bit) {
            restore_lpc_64bit_asm(buffer, vec.num_samples, vec.coefficients, vec.order, vec.shift);
        } else {
            restore_lpc_32bit_asm(buffer, vec.num_samples, vec.coefficients, vec.order, vec.shift);
        }

        // Compare output to expected
        bool match = true;
        uint32_t first_mismatch = 0;
        for (uint32_t j = 0; j < vec.num_samples; j++) {
            if (buffer[j] != vec.expected_output[j]) {
                match = false;
                first_mismatch = j;
                break;
            }
        }

        if (match) {
            pass_count++;
            printf("PASS [%u]: order=%u, shift=%d, bps=%u, samples=%u, %s\n", (unsigned)i,
                   (unsigned)vec.order, (int)vec.shift, (unsigned)vec.bits_per_sample,
                   (unsigned)vec.num_samples, vec.is_64bit ? "64-bit" : "32-bit");
        } else {
            fail_count++;
            printf("FAIL [%u]: order=%u, shift=%d, bps=%u, %s\n", (unsigned)i, (unsigned)vec.order,
                   (int)vec.shift, (unsigned)vec.bits_per_sample,
                   vec.is_64bit ? "64-bit" : "32-bit");
            printf("  First mismatch at sample %u: got %ld, expected %ld\n",
                   (unsigned)first_mismatch, (long)buffer[first_mismatch],
                   (long)vec.expected_output[first_mismatch]);
        }

        free(buffer);
    }

    printf("\n========================================\n");
    printf("Results: %u passed, %u failed, %u total\n", (unsigned)pass_count, (unsigned)fail_count,
           (unsigned)lpc_test_vector_count);
    printf("========================================\n");

    if (fail_count == 0) {
        printf("\nALL TESTS PASSED\n");
    } else {
        printf("\nSOME TESTS FAILED\n");
    }

    // Repeat summary periodically so it's visible if monitor connects late
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        printf("\nResults: %u passed, %u failed, %u total\n", (unsigned)pass_count,
               (unsigned)fail_count, (unsigned)lpc_test_vector_count);
    }
}
