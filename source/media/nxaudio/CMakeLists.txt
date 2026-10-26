# NXAudio CMake Build System
# Copyright (c) 2025 KetiveeAI - KETIVEEAI License

cmake_minimum_required(VERSION 3.15)
project(nxaudio VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Options
option(NXAUDIO_BUILD_DAEMON "Build audio daemon" ON)
option(NXAUDIO_BUILD_TESTS "Build test suite" OFF)
option(NXAUDIO_BUILD_EXAMPLES "Build examples" OFF)
option(NXAUDIO_ENABLE_SIMD "Enable SIMD optimizations" ON)

# Detect platform
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(NXAUDIO_PLATFORM_LINUX ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "NeolyxOS")
    set(NXAUDIO_PLATFORM_NEOLYX ON)
endif()

# Source files
set(NXAUDIO_CORE_SOURCES
    src/core/init.c
)

set(NXAUDIO_SPATIAL_SOURCES
    src/spatial/hrtf.c
    src/spatial/mixer.c
)

set(NXAUDIO_DSP_SOURCES
    src/dsp/engine.c
    src/dsp/stream.c
    src/dsp/resample.c
    src/dsp/mixer.c
    src/dsp/effects.c
    src/metadata/metadata.c
)

set(NXAUDIO_CODEC_SOURCES
    src/codecs/loader.c
    src/codecs/wav.c
    src/codecs/aiff.c
    src/codecs/flac.c
    src/codecs/mp3.c
    src/codecs/ogg.c
)

# Output devices; each file compiles to nothing off its own platform
set(NXAUDIO_DEVICE_SOURCES
    src/device/device.c
    src/device/device_null.c
    src/device/device_pulse.c
    src/device/device_alsa.c
    src/device/device_wasapi.c
    src/device/device_coreaudio.c
)

set(NXAUDIO_ALL_SOURCES
    ${NXAUDIO_CORE_SOURCES}
    ${NXAUDIO_DSP_SOURCES}
    ${NXAUDIO_CODEC_SOURCES}
    ${NXAUDIO_DEVICE_SOURCES}
    ${NXAUDIO_SPATIAL_SOURCES}
)

# Main library
add_library(nxaudio SHARED ${NXAUDIO_ALL_SOURCES})
add_library(nxaudio_static STATIC ${NXAUDIO_ALL_SOURCES})

# Include directories
target_include_directories(nxaudio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(nxaudio_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Platform libraries. PulseAudio and ALSA are dlopen'd, so Linux only
# needs libdl; Windows needs COM for WASAPI; macOS the audio frameworks.
find_package(Threads REQUIRED)
set(NXAUDIO_PLATFORM_LIBS Threads::Threads)
if(WIN32)
    list(APPEND NXAUDIO_PLATFORM_LIBS ole32)
elseif(APPLE)
    list(APPEND NXAUDIO_PLATFORM_LIBS "-framework AudioToolbox" "-framework CoreAudio")
else()
    list(APPEND NXAUDIO_PLATFORM_LIBS m ${CMAKE_DL_LIBS})
endif()

target_link_libraries(nxaudio PRIVATE ${NXAUDIO_PLATFORM_LIBS})
# Static consumers (the browser) need the same libraries at link time
target_link_libraries(nxaudio_static PUBLIC ${NXAUDIO_PLATFORM_LIBS})

# SIMD optimizations
if(NXAUDIO_ENABLE_SIMD)
    include(CheckCCompilerFlag)
    
    check_c_compiler_flag("-mavx2" HAVE_AVX2)
    if(HAVE_AVX2)
        target_compile_options(nxaudio PRIVATE -mavx2)
        target_compile_definitions(nxaudio PRIVATE NXAUDIO_HAVE_AVX2)
    endif()
    
    check_c_compiler_flag("-msse4.1" HAVE_SSE41)
    if(HAVE_SSE41)
        target_compile_options(nxaudio PRIVATE -msse4.1)
        target_compile_definitions(nxaudio PRIVATE NXAUDIO_HAVE_SSE41)
    endif()
endif()

# Version info
target_compile_definitions(nxaudio PRIVATE
    NXAUDIO_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    NXAUDIO_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    NXAUDIO_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# Set library version
set_target_properties(nxaudio PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# Installation
include(GNUInstallDirs)

install(TARGETS nxaudio nxaudio_static
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/nxaudio
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# pkgconfig
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/nxaudio.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/nxaudio.pc
    @ONLY
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nxaudio.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

# Command-line player: nxaudio_play <file.wav>
add_executable(nxaudio_play src/player.c)
target_link_libraries(nxaudio_play PRIVATE nxaudio_static)

# Tests (also built when the parent project enables testing)
if(NXAUDIO_BUILD_TESTS OR BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# Examples
if(NXAUDIO_BUILD_EXAMPLES)
    add_subdirectory(examples EXCLUDE_FROM_ALL)
endif()

# Summary
message(STATUS "")
message(STATUS "NXAudio Configuration:")
message(STATUS "  Version:       ${PROJECT_VERSION}")
message(STATUS "  Platform:      ${CMAKE_SYSTEM_NAME}")
message(STATUS "  SIMD:          ${NXAUDIO_ENABLE_SIMD}")
message(STATUS "  Build daemon:  ${NXAUDIO_BUILD_DAEMON}")
message(STATUS "  Build tests:   ${NXAUDIO_BUILD_TESTS}")
message(STATUS "")
