# Copyright (c) 2025 KetiveeAI. All rights reserved.
# Licensed under KPL-2.0. See LICENSE file for details.
# =============================================================================
# ZepraScript Tests — Global Test Directory
# =============================================================================

find_package(GTest QUIET)

if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# =============================================================================
# Unit Tests
# =============================================================================
set(ZEPRASCRIPT_UNIT_TEST_SOURCES
    unit/value_tests.cpp
    unit/number_conversion_tests.cpp
    unit/lexer_tests.cpp
    unit/parser_tests.cpp
    unit/vm_tests.cpp
    unit/gc_tests.cpp
    unit/builtin_tests.cpp
    unit/url_tests.cpp
    unit/wasm_tests.cpp
    unit/zeprascript_comprehensive_tests.cpp
    unit/async_tests.cpp
    unit/class_tests.cpp
    unit/zir_tests.cpp
    unit/runtime_tests.cpp
    unit/RuntimeTests.cpp
    unit/JITTests.cpp
    unit/WASMTests.cpp
    unit/sandbox_tests.cpp
    unit/robustness_tests.cpp
    unit/stress_tests.cpp
    unit/browser_integration_tests.cpp
    unit/generator_tests.cpp
    unit/vm_stress_test.cpp
    unit/gc_concurrent_stress_test.cpp
    unit/jit_stress_test.cpp
    unit/syntax_p0_tests.cpp
)

add_executable(zeprascript-unit-tests ${ZEPRASCRIPT_UNIT_TEST_SOURCES})

target_link_libraries(zeprascript-unit-tests
    PRIVATE
        zepra-core
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(zeprascript-unit-tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/source/zepraScript
)

include(GoogleTest)
gtest_discover_tests(zeprascript-unit-tests)

# =============================================================================
# Integration Tests
# =============================================================================
set(ZEPRASCRIPT_INTEGRATION_TEST_SOURCES
    integration/module_tests.cpp
    integration/promise_tests.cpp
    integration/fetch_tests.cpp
    integration/indexeddb_tests.cpp
    integration/browser_vm_integration_test.cpp
    integration/js_execution_test.cpp
    integration/script_context_test.cpp
)

add_executable(zeprascript-integration-tests ${ZEPRASCRIPT_INTEGRATION_TEST_SOURCES})

target_link_libraries(zeprascript-integration-tests
    PRIVATE
        zepra-core
        webcore
        nxhttp
        networking
        nxbase
        GTest::gtest
        GTest::gtest_main
)
if(WIN32)
    target_link_libraries(zeprascript-integration-tests PRIVATE
        ${OPENSSL_LIBRARIES} ws2_32)
elseif(APPLE)
    target_link_libraries(zeprascript-integration-tests PRIVATE
        ${OPENSSL_LIBRARIES})
else()
    target_link_libraries(zeprascript-integration-tests PRIVATE
        ssl crypto pthread)
endif()

target_include_directories(zeprascript-integration-tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/source/zepraScript
        ${PROJECT_SOURCE_DIR}/source/webCore
)

target_compile_definitions(zeprascript-integration-tests
    PRIVATE
        SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

gtest_discover_tests(zeprascript-integration-tests)
