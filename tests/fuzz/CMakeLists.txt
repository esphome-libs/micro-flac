cmake_minimum_required(VERSION 3.16)
project(micro_flac_fuzz CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# libFuzzer is the default. Apple clang ships without libclang_rt.fuzzer, so
# point CMAKE_CXX_COMPILER at Homebrew LLVM (e.g. /opt/homebrew/opt/llvm/bin/clang++).
# Pass -DFUZZ_USE_LIBFUZZER=OFF for the standalone harness instead.
option(FUZZ_USE_LIBFUZZER "Build as a libFuzzer target" ON)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Fuzzing requires Clang. Set CMAKE_CXX_COMPILER to clang++.")
endif()

# AppleClang satisfies the MATCHES "Clang" check above but ships without the
# libFuzzer runtime, so a libFuzzer build only fails later at link with a
# confusing error. Fail early with guidance. The standalone build
# (-DFUZZ_USE_LIBFUZZER=OFF) needs only ASan/UBSan and works under AppleClang.
if(FUZZ_USE_LIBFUZZER AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    message(FATAL_ERROR
        "AppleClang has no libFuzzer runtime. Point CMAKE_CXX_COMPILER at Homebrew "
        "LLVM (e.g. $(brew --prefix llvm)/bin/clang++), or pass "
        "-DFUZZ_USE_LIBFUZZER=OFF for the standalone harness.")
endif()

# ASan + UBSan, all checks on. The FLAC decoder is built to be UBSan-clean under
# fuzzing: channel decorrelation and PCM packing route every wrap through the
# uint32_t helpers in src/wrapping_arithmetic.h, and the LPC/fixed restore
# functions carry __attribute__((no_sanitize("signed-integer-overflow"))) for
# their intentional modular-int32 prediction arithmetic (UB per the C standard).
# That attribute only engages when FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION is
# defined (see src/compiler.h, FLAC_NO_SANITIZE_OVERFLOW); defining it below
# leaves the rest of UBSan active, including the shift and bounds checks that
# catch real bitstream bugs. -fno-sanitize-recover makes every finding fatal so
# the fuzzer stops on the first one.
set(FUZZ_FLAGS "-g -fno-omit-frame-pointer -fsanitize=address,undefined")
set(FUZZ_FLAGS "${FUZZ_FLAGS} -fno-sanitize-recover=all")
set(FUZZ_FLAGS "${FUZZ_FLAGS} -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION=1")

if(FUZZ_USE_LIBFUZZER)
    set(FUZZ_FLAGS "${FUZZ_FLAGS} -fsanitize=fuzzer-no-link")
endif()

set(CMAKE_CXX_FLAGS        "${CMAKE_CXX_FLAGS} ${FUZZ_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${FUZZ_FLAGS}")

# On macOS with Homebrew LLVM, link against LLVM's libc++ so the sanitizer
# runtime and the C++ library agree. The clang++ driver already adds -lc++
# implicitly; the -L/-rpath just steers it to LLVM's copy over the system one.
if(APPLE)
    get_filename_component(LLVM_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(LLVM_PREFIX  "${LLVM_BIN_DIR}" DIRECTORY)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L${LLVM_PREFIX}/lib/c++ -Wl,-rpath,${LLVM_PREFIX}/lib/c++")
endif()

# Build the library (and the Ogg FLAC demuxer it links) from the repo root so
# their translation units pick up the sanitizer/coverage instrumentation set in
# CMAKE_CXX_FLAGS above. Ogg FLAC support stays ON (the default) so the demuxer
# path is fuzzed too.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. micro-flac-build)

# --- FLAC streaming fuzzer --------------------------------------------------
add_executable(fuzz_flac_decode fuzz_flac_decode.cpp)
target_link_libraries(fuzz_flac_decode PRIVATE micro_flac)

if(FUZZ_USE_LIBFUZZER)
    target_compile_options(fuzz_flac_decode PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_flac_decode    PRIVATE -fsanitize=fuzzer)
else()
    target_compile_definitions(fuzz_flac_decode PRIVATE FUZZ_STANDALONE=1)
endif()
