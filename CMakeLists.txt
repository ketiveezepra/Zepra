cmake_minimum_required(VERSION 3.16)
project(Zepra VERSION 0.1.0 LANGUAGES CXX)

# ============================================================================
# Build System - Prefer Ninja for faster, lighter builds
# ============================================================================
# To use Ninja: cmake -G Ninja -B build
# Or set CMAKE_GENERATOR=Ninja environment variable
# ============================================================================

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Performance optimizations for build
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # For IDE support

# Limit parallel jobs to prevent system overload (adjust based on your system)
if(NOT DEFINED CMAKE_BUILD_PARALLEL_LEVEL)
    # Use half of available cores to prevent overload
    cmake_host_system_information(RESULT NPROC QUERY NUMBER_OF_LOGICAL_CORES)
    math(EXPR PARALLEL_JOBS "${NPROC} / 2")
    if(PARALLEL_JOBS LESS 1)
        set(PARALLEL_JOBS 1)
    endif()
    message(STATUS "Build parallelism: ${PARALLEL_JOBS} jobs (of ${NPROC} cores)")
endif()

# Ninja-specific optimizations
if(CMAKE_GENERATOR MATCHES "Ninja")
    message(STATUS "Using Ninja generator - optimized for performance")
    # Ninja pool for linking (reduce memory usage)
    set_property(GLOBAL PROPERTY JOB_POOLS link_pool=2)
endif()

# Build options
option(USE_UNIFIED_SYSTEM "Use unified Zepra Core system" OFF)
option(USE_NXRENDER "Use NXRENDER native rendering (no SDL)" OFF)
option(USE_ZEPRA_STACK "Use custom Zepra libraries (NxHTTP, NxJSON, NxCrypto)" OFF)

# Version metadata
set(ZEPRA_VERSION_SUFFIX "beta" CACHE STRING "Version suffix (beta, nightly, rc1, etc.)")
set(ZEPRA_BUILD_CHANNEL "beta" CACHE STRING "Build channel (stable, beta, nightly)")
string(TIMESTAMP ZEPRA_BUILD_DATE "%Y-%m-%d" UTC)
string(TIMESTAMP ZEPRA_BUILD_YEAR "%Y" UTC)
set(ZEPRA_VERSION_STRING "${PROJECT_VERSION}-${ZEPRA_VERSION_SUFFIX}")
set(ZEPRA_IS_BETA TRUE)

# Generate version header
configure_file(
    "${CMAKE_SOURCE_DIR}/source/version.h.in"
    "${CMAKE_BINARY_DIR}/generated/version.h"
    @ONLY
)
include_directories(${CMAKE_BINARY_DIR}/generated)

# Set build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Platform-specific settings
if(WIN32)
    # Windows-specific settings
    add_definitions(-DWIN32_LEAN_AND_MEAN)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    
    # Try to find SDL2 using different methods
    find_package(SDL2 QUIET)
    if(NOT SDL2_FOUND)
        # Try to find SDL2 using pkg-config (if available)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(SDL2 QUIET sdl2)
        endif()
    endif()
    

    
    # Try to find OpenSSL on Windows
    find_package(OpenSSL QUIET)
    if(NOT OPENSSL_FOUND)
        find_path(OPENSSL_INCLUDE_DIR openssl/ssl.h)
        find_library(OPENSSL_CRYPTO_LIBRARY crypto)
        find_library(OPENSSL_SSL_LIBRARY ssl)
        if(OPENSSL_INCLUDE_DIR AND OPENSSL_CRYPTO_LIBRARY AND OPENSSL_SSL_LIBRARY)
            set(OPENSSL_FOUND TRUE)
            set(OPENSSL_INCLUDE_DIRS ${OPENSSL_INCLUDE_DIR})
            set(OPENSSL_LIBRARIES ${OPENSSL_SSL_LIBRARY} ${OPENSSL_CRYPTO_LIBRARY})
        else()
            message(WARNING "OpenSSL not found on Windows — TLS will be unavailable")
        endif()
    endif()
    
    # Link Windows libraries
    set(PLATFORM_LIBS ws2_32 wininet opengl32 gdi32 shell32)
    
elseif(APPLE)
    # macOS-specific settings
    find_package(SDL2 REQUIRED)

    set(PLATFORM_LIBS "")
    
else()
    # Linux-specific settings
    find_package(PkgConfig REQUIRED)
    find_package(X11 REQUIRED)
    find_package(Freetype REQUIRED)
    
    if(NOT USE_NXRENDER)
        pkg_check_modules(SDL2 REQUIRED sdl2)
        pkg_check_modules(SDL2_IMAGE REQUIRED SDL2_image)
        pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)
    endif()

    pkg_check_modules(OPENSSL REQUIRED openssl)
    set(PLATFORM_LIBS ${X11_LIBRARIES} Freetype::Freetype)
endif()

# Find OpenGL
if(NOT WIN32)
    find_package(OpenGL REQUIRED)
else()
    # On Windows, opengl32 is linked directly via PLATFORM_LIBS
    set(OPENGL_LIBRARIES "")
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/source/integration/include)
include_directories(${CMAKE_SOURCE_DIR}/external)

# NXNet includes
include_directories(${CMAKE_SOURCE_DIR}/source/NXNet)
include_directories(${CMAKE_SOURCE_DIR}/source/NXNet/networking/include)
include_directories(${CMAKE_SOURCE_DIR}/source/NXNet/nxbase/include
                    ${CMAKE_SOURCE_DIR}/source/NXNet/nxbase)
include_directories(${CMAKE_SOURCE_DIR}/source/NXNet/nxhttp/include)
include_directories(${CMAKE_SOURCE_DIR}/source/NXNet/nxcrypto/include)

# ============================================================================
# Engine subdirectories (zepraScript, webCore, etc.)
# ============================================================================
# Build ZepraScript JS engine (creates zepra-core library)
add_subdirectory(source/zepraScript)
include_directories(${CMAKE_SOURCE_DIR}/source/zepraScript)

# Build WebCore rendering engine (creates webcore library)
add_subdirectory(source/webCore)
include_directories(${CMAKE_SOURCE_DIR}/source/webCore/include)

# Build Networking module (Now part of NXNet submodule)

# Build NXRender-cpp graphics library
add_subdirectory(source/nxrender-cpp)

# NOTE: zepraEngine demos moved to test/demos/zepraEngine/ (SDL removed)

# Build Integration layer (JS↔DOM bridge)
add_subdirectory(source/integration)
include_directories(${CMAKE_SOURCE_DIR}/source/integration/include)

# Build Platform abstraction (window system, clipboard, platform_compat.h)
add_subdirectory(source/platform)
include_directories(${CMAKE_SOURCE_DIR}/source/platform/include)

# Build NXAudio (Audio Engine)
add_subdirectory(source/media/nxaudio)
include_directories(${CMAKE_SOURCE_DIR}/source/media/nxaudio/include)

# ============================================================================
# Custom Zepra Stack (optional - replace system libs)
# ============================================================================
# Custom Zepra Stack (Unconditional)
# ============================================================================
    message(STATUS "Using custom Zepra Stack libraries")
    add_subdirectory(source/NXNet)
    add_subdirectory(source/nxjson)
    add_subdirectory(source/nxxml)
    
    include_directories(${CMAKE_SOURCE_DIR}/source/nxjson/include)
    include_directories(${CMAKE_SOURCE_DIR}/source/nxjson)
    include_directories(${CMAKE_SOURCE_DIR}/source/nxxml/include)

# Source files
if(USE_NXRENDER)
    # NXRENDER native build - no SDL
    set(SOURCES
        source/main.cpp
        source/integration/zepra_browser.cpp
        source/zepraEngine/engine/dev_tools.cpp
        source/zepraEngine/engine/html_parser_stub.cpp
        source/zepraEngine/media/audio_equalizer.cpp
        source/zepraEngine/media/browser_audio.cpp
        source/zepraEngine/media/audio_codec_detector.cpp
        source/zepraEngine/media/media_pipeline.cpp
        source/zepraEngine/media/video_player.cpp
        source/zepraEngine/media/video_processor.cpp
        source/zepraEngine/media/html_video_element.cpp
        source/zepraEngine/engine/console_log.cpp
        source/zepraEngine/engine/download_manager.cpp
        source/aiEngine/src/search/ketivee_search.cpp
        UI/browser/window.cpp
        UI/browser/tab_manager.cpp
        UI/devtools/dev_tools_ui.cpp
        source/platform/src/auth/zepra_auth.cpp
        UI/settings/auth_ui.cpp
        UI/settings/settings_ui.cpp
        source/sandbox/src/sandbox_manager.cpp
        source/platform/src/config/config_manager.cpp
        source/NXNet/networking/remote/zep_remote.cpp
        source/NXNet/networking/network/network_monitor.cpp
        source/NXNet/networking/network/monitored_http.cpp
        source/sandbox/src/security/security_checker.cpp
        source/webCore/browser/mouse_handler.cpp
        source/webCore/browser/layout_engine.cpp
        source/webCore/browser/css_utils.cpp
        # New modular architecture
        source/storage/src/bookmark_manager.cpp
        source/storage/src/history_database.cpp
        source/storage/src/settings_store.cpp
        source/webCore/browser/navigation_controller.cpp
        source/webCore/browser/rewards_manager.cpp
        source/aiEngine/src/ai_sidebar.cpp
        # Memory optimization
        source/platform/src/memory/memory_pools.cpp
        source/webCore/browser/tab_suspender.cpp
        source/webCore/browser/lazy_image_loader.cpp
        # WebCore integration
        source/integration/webcore_integration.cpp
        # NXRender-Layout bridge
        source/webCore/browser/nxrender_layout_bridge.cpp
        # Stubs for missing WebCore symbols (WebGL, etc.)
        source/integration/stubs/webgl_stubs.cpp
        # tab_suspender_stubs removed â€” real impl now active
        source/integration/stubs/link_stubs.cpp
    )
    set(EXECUTABLE_NAME "zepra_browser")
    # NXRender-cpp is built as subdirectory, no external lib path needed
elseif(USE_UNIFIED_SYSTEM)
    set(SOURCES
        source/main.cpp
        source/zepraEngine/core/zepra_core.cpp
        source/zepraEngine/core/ui_elements.cpp
        source/zepraEngine/engine/dev_tools.cpp
        source/zepraEngine/engine/download_manager.cpp
        source/aiEngine/src/search/ketivee_search.cpp
        UI/browser/window.cpp
        UI/browser/tab_manager.cpp
        UI/devtools/dev_tools_ui.cpp
        source/platform/src/auth/zepra_auth.cpp
        UI/settings/auth_ui.cpp
        UI/settings/settings_ui.cpp
        source/sandbox/src/sandbox_manager.cpp
        source/platform/src/config/config_manager.cpp
        source/platform/src/platform_infrastructure.cpp
    )
    set(EXECUTABLE_NAME "zepra_core_browser")
else()
    set(SOURCES
        source/main.cpp
        source/integration/zepra_browser.cpp
        source/zepraEngine/engine/dev_tools.cpp
        source/zepraEngine/media/audio_equalizer.cpp
        source/zepraEngine/media/browser_audio.cpp
        source/zepraEngine/media/audio_codec_detector.cpp
        source/zepraEngine/media/media_pipeline.cpp
        source/zepraEngine/media/video_player.cpp
        source/zepraEngine/media/video_processor.cpp
        source/zepraEngine/media/html_video_element.cpp
        source/zepraEngine/engine/html_parser_stub.cpp
        source/zepraEngine/engine/download_manager.cpp
        source/aiEngine/src/search/ketivee_search.cpp
        UI/browser/window.cpp
        UI/browser/tab_manager.cpp
        UI/devtools/dev_tools_ui.cpp
        source/platform/src/auth/zepra_auth.cpp
        UI/settings/auth_ui.cpp
        UI/settings/settings_ui.cpp
        source/sandbox/src/sandbox_manager.cpp
        source/platform/src/config/config_manager.cpp
        source/NXNet/networking/network/network_monitor.cpp
    )


    set(EXECUTABLE_NAME "zepra_browser")
endif()

# Ensure SOURCES is defined
if(NOT DEFINED SOURCES)
    message(FATAL_ERROR "SOURCES variable is not defined")
endif()

# Header files
set(HEADERS
    source/integration/common/types.h
    source/integration/common/constants.h
    source/zepraEngine/core/zepra_core.h
    source/zepraEngine/engine/html_parser.h
    source/zepraEngine/engine/dev_tools.h
    source/zepraEngine/engine/download_manager.h
    source/aiEngine/include/search/ketivee_search.h
    UI/browser/window.h
    UI/browser/tab_manager.h
    UI/devtools/dev_tools_ui.h
    source/platform/include/auth/zepra_auth.h
    UI/settings/auth_ui.h
    UI/settings/settings_ui.h
    source/NXNet/networking/http.h
    source/sandbox/include/sandbox/sandbox_manager.h
    source/platform/include/config/dual_config.h
    source/platform/src/config/config_manager.h
)

# Create main executable
add_executable(${EXECUTABLE_NAME} ${SOURCES} ${HEADERS})

# On Windows Release builds, hide console window (use WinMain entry)
if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_target_properties(${EXECUTABLE_NAME} PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
endif()

# Link libraries based on build mode
if(USE_NXRENDER)
    # NXRENDER mode - link nxrender + platform-specific graphics

    # Common libraries for all platforms
    target_link_libraries(${EXECUTABLE_NAME}
        webcore
        nxrender
        zepra-integration
        networking
        nxaudio_static
        nxhttp
        nxbase
        nxcrypto
        nxjson
        nxxml
    )

    if(WIN32)
        # Windows: OpenGL + Win32 APIs (no X11, no SDL2)
        find_package(OpenGL REQUIRED)
        find_package(Freetype REQUIRED)
        target_link_libraries(${EXECUTABLE_NAME}
            OpenGL::GL
            Freetype::Freetype
            user32 gdi32 opengl32 ws2_32
        )
    else()
        # Linux: X11/GLX/FreeType/SDL2 as before
        find_package(X11 REQUIRED)
        find_package(Freetype REQUIRED)
        find_package(PkgConfig REQUIRED)
        target_link_libraries(${EXECUTABLE_NAME}
            ${X11_LIBRARIES}
            X11
            GL GLX
            pthread dl m z
            Freetype::Freetype
            SDL2
        )
        target_include_directories(${EXECUTABLE_NAME} PRIVATE
            ${X11_INCLUDE_DIR}
            ${FREETYPE_INCLUDE_DIRS}
        )
    endif()

    target_include_directories(${EXECUTABLE_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/source/integration/include
        ${CMAKE_SOURCE_DIR}/source/storage/include/storage
        ${CMAKE_SOURCE_DIR}/source/webCore/include/webcore/browser
        ${CMAKE_SOURCE_DIR}/source/platform/include/input
        ${CMAKE_SOURCE_DIR}/source/aiEngine/include
        ${CMAKE_SOURCE_DIR}/source/nxrender-cpp/include
        ${CMAKE_SOURCE_DIR}/source/zepraEngine
        ${CMAKE_SOURCE_DIR}/source/webCore/html
        ${CMAKE_SOURCE_DIR}/source/webCore
        ${CMAKE_SOURCE_DIR}/source/platform/include
        ${CMAKE_SOURCE_DIR}/source/sandbox/include
        ${CMAKE_SOURCE_DIR}/source/storage/include
        ${CMAKE_SOURCE_DIR}/source/networking
        ${CMAKE_SOURCE_DIR}/UI
    )
    target_compile_definitions(${EXECUTABLE_NAME} PRIVATE USE_WEBCORE
        RESOURCE_PATH="${CMAKE_SOURCE_DIR}/resources"
    )
    message(STATUS "Using NXRENDER native rendering (platform: ${CMAKE_SYSTEM_NAME})")
    message(STATUS "Using NXRENDER + REOX native rendering with X11/OpenGL/FreeType (NO SDL)")
else()
    # SDL mode
    if(SDL2_FOUND)
        if(SDL2_LIBRARIES)
            target_link_libraries(${EXECUTABLE_NAME} ${SDL2_LIBRARIES})
            target_include_directories(${EXECUTABLE_NAME} PRIVATE ${SDL2_INCLUDE_DIRS})
        else()
            target_link_libraries(${EXECUTABLE_NAME} SDL2::SDL2)
        endif()
        # USE_WEBCORE needed for gfx namespace in zepra_browser.cpp
        target_compile_definitions(${EXECUTABLE_NAME} PRIVATE USE_WEBCORE)
    else()
        message(FATAL_ERROR "SDL2 not found. Use -DUSE_NXRENDER=ON for native rendering.")
    endif()
endif()

# SDL2 image/ttf only needed when NOT using NXRender
if(NOT USE_NXRENDER)
    if(SDL2_IMAGE_FOUND)
        target_link_libraries(${EXECUTABLE_NAME} ${SDL2_IMAGE_LIBRARIES})
        target_include_directories(${EXECUTABLE_NAME} PRIVATE ${SDL2_IMAGE_INCLUDE_DIRS})
    else()
        message(STATUS "SDL2_image not found via pkg-config")
    endif()

    if(SDL2_TTF_FOUND)
        target_link_libraries(${EXECUTABLE_NAME} ${SDL2_TTF_LIBRARIES})
        target_include_directories(${EXECUTABLE_NAME} PRIVATE ${SDL2_TTF_INCLUDE_DIRS})
    else()
        message(STATUS "SDL2_ttf not found via pkg-config")
    endif()
endif()




if(OPENSSL_FOUND)
    if(OPENSSL_LIBRARIES)
        target_link_libraries(${EXECUTABLE_NAME} ${OPENSSL_LIBRARIES})
        target_include_directories(${EXECUTABLE_NAME} PRIVATE ${OPENSSL_INCLUDE_DIRS})
    else()
        target_link_libraries(${EXECUTABLE_NAME} OpenSSL::SSL OpenSSL::Crypto)
    endif()
else()
    message(FATAL_ERROR "OpenSSL not found. Please install OpenSSL development libraries.")
endif()

# nlohmann/json.hpp is header-only, no linking needed
# The include path is already set up in include_directories

# json-c (used by zepra_auth — Linux/NeolyxOS only)
find_package(PkgConfig QUIET)
if(NOT WIN32 AND PkgConfig_FOUND)
    pkg_check_modules(JSON_C QUIET json-c)
    if(JSON_C_FOUND)
        target_link_libraries(${EXECUTABLE_NAME} ${JSON_C_LIBRARIES})
        target_include_directories(${EXECUTABLE_NAME} PRIVATE ${JSON_C_INCLUDE_DIRS})
    endif()
endif()

# Link OpenGL and platform libraries
target_link_libraries(${EXECUTABLE_NAME} ${OPENGL_LIBRARIES} ${PLATFORM_LIBS})

# zlib (used by tab_suspender, nxpdf)
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_link_libraries(${EXECUTABLE_NAME} ZLIB::ZLIB)
elseif(NOT WIN32)
    # On Linux/macOS, zlib is always available as -lz
    target_link_libraries(${EXECUTABLE_NAME} z)
else()
    message(WARNING "ZLIB not found on Windows — PDF and compression features will be unavailable. Install zlib via vcpkg.")
endif()

# Link engine libraries (zepraScript, webCore, integration, platform)
# Use linker group for static libs to resolve circular dependencies
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_libraries(${EXECUTABLE_NAME}
        -Wl,--start-group
        zepra-core
        webcore
        zepra-integration
        platform
        nxhttp
        networking
        nxbase
        nxcrypto
        nxjson
        nxxml
        -Wl,--end-group
    )
else()
    target_link_libraries(${EXECUTABLE_NAME}
        zepra-core
        webcore
        zepra-integration
        platform
        nxhttp
        networking
        nxbase
        nxcrypto
        nxjson
        nxxml
    )
endif()

# OpenSSL (needed by nxhttp/networking for TLS)
if(OPENSSL_FOUND)
    target_link_libraries(${EXECUTABLE_NAME} ${OPENSSL_LIBRARIES})
endif()

# Compiler flags
target_compile_options(${EXECUTABLE_NAME} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

# MinGW: statically link runtime DLLs (libgcc, libstdc++, libwinpthread)
# This eliminates the need to ship these DLLs alongside the executable
if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_link_options(${EXECUTABLE_NAME} PRIVATE
        -static-libgcc -static-libstdc++
    )
    # Also try to statically link winpthread if available
    target_link_options(${EXECUTABLE_NAME} PRIVATE
        $<$<BOOL:TRUE>:-Wl,-Bstatic -lwinpthread -Wl,-Bdynamic>
    )
    message(STATUS "MinGW detected: statically linking libgcc/libstdc++/winpthread")
endif()

# Post-build: copy required DLLs to output directory (OpenSSL, FreeType, etc.)
if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    # Find the MinGW bin directory from the compiler path
    get_filename_component(MINGW_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
    set(RUNTIME_DLLS
        "libssl-3-x64.dll"
        "libcrypto-3-x64.dll"
        "libfreetype-6.dll"
        "libharfbuzz-0.dll"
        "zlib1.dll"
        "libpng16-16.dll"
        "libbz2-1.dll"
        "libbrotlicommon.dll"
        "libbrotlidec.dll"
        "libgraphite2.dll"
        "libintl-8.dll"
        "libiconv-2.dll"
        "libglib-2.0-0.dll"
        "libpcre2-8-0.dll"
    )
    foreach(DLL ${RUNTIME_DLLS})
        if(EXISTS "${MINGW_BIN_DIR}/${DLL}")
            add_custom_command(TARGET ${EXECUTABLE_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MINGW_BIN_DIR}/${DLL}"
                    "$<TARGET_FILE_DIR:${EXECUTABLE_NAME}>"
                COMMENT "Copying ${DLL}"
            )
        endif()
    endforeach()
endif()


# Installation rules
install(TARGETS ${EXECUTABLE_NAME}
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES ${HEADERS}
    DESTINATION include/zepra
)

# Package configuration
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ZepraConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

# Documentation
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    "${CMAKE_CURRENT_BINARY_DIR}/README.md"
    COPYONLY
)

# Print configuration summary
message(STATUS "Zepra Browser Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  SDL2 found: ${SDL2_FOUND}")
message(STATUS "  OpenGL found: ${OPENGL_FOUND}")
message(STATUS "  HTTP Client: nxhttp (libcurl removed)")
message(STATUS "  Platform: ${CMAKE_SYSTEM_NAME}")



# Tests
add_subdirectory(test)
