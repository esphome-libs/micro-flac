cmake_minimum_required(VERSION 3.16)
project(wav_to_flac CXX)

# Option to enable sanitizers
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(ENABLE_SANITIZERS)
    message(STATUS "Building with AddressSanitizer and UndefinedBehaviorSanitizer")
    set(SANITIZER_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer -g")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_FLAGS}")
endif()

# Add microFLAC as a subdirectory (goes up two levels to root)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_CURRENT_BINARY_DIR}/micro-flac)

# Check the encoder's Pass A overflow proof at run time: its partial sums
# widen to 64 bits and trap if any passes 32 (sum_chunk_samples() in
# src/flac_encoder.cpp). An overflow there would not change the encoded
# bytes, so no output comparison can catch it. The round-trip suite drives
# this build, so it runs checked.
target_compile_definitions(micro_flac PRIVATE MICRO_FLAC_CHECK_SCAN_SUMS)

# ------------------------------------------------------------------------------
# wav_to_flac: the encoder CLI. Reads an 8/16/24-bit PCM WAV
# file (1-8 channels) and encodes it to native FLAC via FLACEncoder.
# ------------------------------------------------------------------------------
add_executable(wav_to_flac src/wav_to_flac.cpp)

target_link_libraries(wav_to_flac PRIVATE micro_flac)

# flac_format.h (CHANNEL_LEFT_SIDE/CHANNEL_RIGHT_SIDE/CHANNEL_MID_SIDE) is an
# internal header under src/, not part of microFLAC's installed public
# include/ tree; the CLI's assignment histogram needs an explicit path to it.
# md5.h, shared with flac_to_wav: the CLI computes the stream's MD5 signature
# itself and writes it into the finished STREAMINFO, since the library leaves
# it unset.
target_include_directories(wav_to_flac PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/include
)

# Install target
install(TARGETS wav_to_flac RUNTIME DESTINATION bin)

target_compile_options(wav_to_flac PRIVATE
    -O2
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
