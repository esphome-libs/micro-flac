cmake_minimum_required(VERSION 3.16)
project(micro_flac_encoder_tests CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sanitizers on by default, as in tests/regression.
option(ENABLE_SANITIZERS "Build with AddressSanitizer and UBSan" ON)
if(ENABLE_SANITIZERS)
    add_compile_options(-g -fno-omit-frame-pointer -fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. micro-flac-build)

# Check the encoder's Pass A overflow proof at run time: its partial sums
# widen to 64 bits and trap if any passes 32 (sum_chunk_samples() in
# src/flac_encoder.cpp). An overflow there would not change the encoded
# bytes, so no output comparison can catch it.
target_compile_definitions(micro_flac PRIVATE MICRO_FLAC_CHECK_SCAN_SUMS)

set(TEST_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wmissing-prototypes>
    $<$<CXX_COMPILER_ID:GNU>:-Wmissing-declarations>
    $<$<BOOL:${ENABLE_WERROR}>:-Werror>
)

# bit_writer.h and bit_reader.h are internal headers under src/.
add_executable(test_bit_writer test_bit_writer.cpp)
target_link_libraries(test_bit_writer PRIVATE micro_flac)
target_include_directories(test_bit_writer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_compile_options(test_bit_writer PRIVATE -O2 ${TEST_WARNINGS})

# -fno-access-control (GCC and Clang) turns off access checking for this
# translation unit only, so the test can read and set FLACEncoder's private
# members without the public header carrying test-only accessors. The
# library itself is built without it. The stereo search's joint
# autocorrelation is checked through the internal lpc_analysis.h.
add_executable(test_encoder_config test_encoder_config.cpp)
target_link_libraries(test_encoder_config PRIVATE micro_flac)
target_include_directories(test_encoder_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_compile_options(test_encoder_config PRIVATE -fno-access-control -O2 ${TEST_WARNINGS})

enable_testing()
add_test(NAME bit_writer COMMAND test_bit_writer)
add_test(NAME encoder_config COMMAND test_encoder_config)
