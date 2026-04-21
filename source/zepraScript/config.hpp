#pragma once

/**
 * @file config.hpp
 * @brief ZepraScript engine configuration and platform detection
 */

#include <cstdint>
#include <cstddef>

// =============================================================================
// Version Information
// =============================================================================
#ifndef ZEPRA_VERSION
#define ZEPRA_VERSION "1.2.0"
#endif

#ifndef ZEPRA_VERSION_MAJOR
#define ZEPRA_VERSION_MAJOR 1
#endif

#ifndef ZEPRA_VERSION_MINOR
#define ZEPRA_VERSION_MINOR 2
#endif

#ifndef ZEPRA_VERSION_PATCH
#define ZEPRA_VERSION_PATCH 0
#endif

// =============================================================================
// Platform Detection
// =============================================================================
#if defined(_WIN32) || defined(_WIN64)
    #define ZEPRA_PLATFORM_WINDOWS 1
    #define ZEPRA_PLATFORM_POSIX 0
#elif defined(__linux__)
    #define ZEPRA_PLATFORM_LINUX 1
    #define ZEPRA_PLATFORM_POSIX 1
#elif defined(__APPLE__)
    #define ZEPRA_PLATFORM_MACOS 1
    #define ZEPRA_PLATFORM_POSIX 1
#elif defined(__FreeBSD__)
    #define ZEPRA_PLATFORM_FREEBSD 1
    #define ZEPRA_PLATFORM_POSIX 1
#else
    #define ZEPRA_PLATFORM_UNKNOWN 1
    #define ZEPRA_PLATFORM_POSIX 0
#endif

// =============================================================================
// Architecture Detection
// =============================================================================
#if defined(__x86_64__) || defined(_M_X64)
    #define ZEPRA_ARCH_X64 1
    #define ZEPRA_ARCH_64BIT 1
#elif defined(__i386__) || defined(_M_IX86)
    #define ZEPRA_ARCH_X86 1
    #define ZEPRA_ARCH_32BIT 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define ZEPRA_ARCH_ARM64 1
    #define ZEPRA_ARCH_64BIT 1
#elif defined(__arm__) || defined(_M_ARM)
    #define ZEPRA_ARCH_ARM 1
    #define ZEPRA_ARCH_32BIT 1
#else
    #define ZEPRA_ARCH_UNKNOWN 1
#endif

// =============================================================================
// Compiler Detection
// =============================================================================
#if defined(__clang__)
    #define ZEPRA_COMPILER_CLANG 1
    #define ZEPRA_COMPILER_VERSION (__clang_major__ * 100 + __clang_minor__)
#elif defined(__GNUC__)
    #define ZEPRA_COMPILER_GCC 1
    #define ZEPRA_COMPILER_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)
#elif defined(_MSC_VER)
    #define ZEPRA_COMPILER_MSVC 1
    #define ZEPRA_COMPILER_VERSION _MSC_VER
#else
    #define ZEPRA_COMPILER_UNKNOWN 1
#endif

// =============================================================================
// Feature Toggles
// =============================================================================
#ifndef ZEPRA_ENABLE_JIT
    #define ZEPRA_ENABLE_JIT 0  // Disabled until JIT is implemented
#endif

#ifndef ZEPRA_ENABLE_DEBUG
    #ifdef NDEBUG
        #define ZEPRA_ENABLE_DEBUG 0
    #else
        #define ZEPRA_ENABLE_DEBUG 1
    #endif
#endif

#ifndef ZEPRA_ENABLE_PROFILING
    #define ZEPRA_ENABLE_PROFILING ZEPRA_ENABLE_DEBUG
#endif

#ifndef ZEPRA_ENABLE_CONSOLE
    #define ZEPRA_ENABLE_CONSOLE 1
#endif

// =============================================================================
// GC Configuration (Optimized for low memory)
// =============================================================================
#ifndef ZEPRA_GC_INITIAL_HEAP_SIZE
    #define ZEPRA_GC_INITIAL_HEAP_SIZE (1 * 1024 * 1024)  // 1 MB (reduced from 4 MB)
#endif

#ifndef ZEPRA_GC_MAX_HEAP_SIZE
    #define ZEPRA_GC_MAX_HEAP_SIZE (256 * 1024 * 1024)  // 256 MB (reduced from 512 MB)
#endif

#ifndef ZEPRA_GC_THRESHOLD_RATIO
    #define ZEPRA_GC_THRESHOLD_RATIO 0.70  // Trigger GC at 70% heap usage
#endif

// =============================================================================
// Stack Configuration (Optimized for low memory)
// =============================================================================
#ifndef ZEPRA_MAX_CALL_STACK_DEPTH
    #define ZEPRA_MAX_CALL_STACK_DEPTH 2048  // Reduced from 10000
#endif

#ifndef ZEPRA_MAX_OPERAND_STACK_SIZE
    #define ZEPRA_MAX_OPERAND_STACK_SIZE 16384  // Reduced from 65536
#endif

// =============================================================================
// Object Pool Configuration
// =============================================================================
#ifndef ZEPRA_OBJECT_POOL_SIZE
    #define ZEPRA_OBJECT_POOL_SIZE 256  // Pre-allocate 256 objects per pool
#endif

#ifndef ZEPRA_STRING_POOL_SIZE
    #define ZEPRA_STRING_POOL_SIZE 512  // Strings are common, larger pool
#endif

#ifndef ZEPRA_ARRAY_POOL_SIZE
    #define ZEPRA_ARRAY_POOL_SIZE 128
#endif

// =============================================================================
// String Configuration
// =============================================================================
#ifndef ZEPRA_MAX_STRING_LENGTH
    #define ZEPRA_MAX_STRING_LENGTH (1024 * 1024 * 1024)  // 1 GB
#endif

#ifndef ZEPRA_STRING_INTERN_TABLE_SIZE
    #define ZEPRA_STRING_INTERN_TABLE_SIZE 4096
#endif

// =============================================================================
// Utility Macros
// =============================================================================
#define ZEPRA_UNUSED(x) (void)(x)

#define ZEPRA_STRINGIFY(x) #x
#define ZEPRA_STRINGIFY_IMPL(x) ZEPRA_STRINGIFY(x)

#if ZEPRA_COMPILER_GCC || ZEPRA_COMPILER_CLANG
    #define ZEPRA_LIKELY(x) __builtin_expect(!!(x), 1)
    #define ZEPRA_UNLIKELY(x) __builtin_expect(!!(x), 0)
    #define ZEPRA_ALWAYS_INLINE __attribute__((always_inline)) inline
    #define ZEPRA_NEVER_INLINE __attribute__((noinline))
    #define ZEPRA_UNREACHABLE() __builtin_unreachable()
    #define ZEPRA_PACKED __attribute__((packed))
#elif ZEPRA_COMPILER_MSVC
    #define ZEPRA_LIKELY(x) (x)
    #define ZEPRA_UNLIKELY(x) (x)
    #define ZEPRA_ALWAYS_INLINE __forceinline
    #define ZEPRA_NEVER_INLINE __declspec(noinline)
    #define ZEPRA_UNREACHABLE() __assume(0)
    #define ZEPRA_PACKED
#else
    #define ZEPRA_LIKELY(x) (x)
    #define ZEPRA_UNLIKELY(x) (x)
    #define ZEPRA_ALWAYS_INLINE inline
    #define ZEPRA_NEVER_INLINE
    #define ZEPRA_UNREACHABLE()
    #define ZEPRA_PACKED
#endif

// Export/Import macros for shared libraries
#if ZEPRA_PLATFORM_WINDOWS
    #ifdef ZEPRA_BUILDING_SHARED
        #define ZEPRA_API __declspec(dllexport)
    #else
        #define ZEPRA_API __declspec(dllimport)
    #endif
#else
    #define ZEPRA_API __attribute__((visibility("default")))
#endif

namespace Zepra {

// Type aliases for consistent sizing across platforms
using int8 = std::int8_t;
using int16 = std::int16_t;
using int32 = std::int32_t;
using int64 = std::int64_t;
using uint8 = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;

using size_t = std::size_t;
using ptrdiff_t = std::ptrdiff_t;

} // namespace Zepra
