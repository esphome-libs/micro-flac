cmake_minimum_required(VERSION 3.16)
project(flac_to_wav CXX)

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

# Create the executable
add_executable(flac_to_wav src/flac_to_wav.cpp)

# Link against microFLAC library
target_link_libraries(flac_to_wav PRIVATE micro_flac)

# Include directories for md5.h
target_include_directories(flac_to_wav PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Install target
install(TARGETS flac_to_wav RUNTIME DESTINATION bin)

# Warning flags for the flac_to_wav executable itself (separate from library flags in cmake/host.cmake)
target_compile_options(flac_to_wav PRIVATE
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
    $<$<BOOL:${ENABLE_WERROR}>:-Werror>
)
