cmake_minimum_required(VERSION 3.16)
project(micro_flac_regression CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sanitizers on by default: these tests exist to catch streaming state-machine
# regressions, and ASan/UBSan turn subtle bookkeeping mistakes into hard failures.
option(ENABLE_SANITIZERS "Build with AddressSanitizer and UBSan" ON)
if(ENABLE_SANITIZERS)
    add_compile_options(-g -fno-omit-frame-pointer -fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. micro-flac-build)

add_executable(test_error_recovery test_error_recovery.cpp)
target_link_libraries(test_error_recovery PRIVATE micro_flac)
# The test builds synthetic FLAC frames, so it needs the internal CRC helpers.
target_include_directories(test_error_recovery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

enable_testing()
add_test(NAME error_recovery COMMAND test_error_recovery)
