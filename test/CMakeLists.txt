
# Test Directory CMake Configuration
message(STATUS "Configuring tests...")

# =============================================================================
# Legacy test targets (disabled — source paths need migration from src/ to source/)
# =============================================================================
if(FALSE)

# Test 1: Zepra Basic Test
add_executable(zepra_test test_basic.cpp 
    ${PROJECT_SOURCE_DIR}/src/search/ketivee_search.cpp
    ${PROJECT_SOURCE_DIR}/src/engine/html_parser_stub.cpp
)
if(SDL2_FOUND)
    if(SDL2_LIBRARIES)
        target_link_libraries(zepra_test ${SDL2_LIBRARIES})
        target_include_directories(zepra_test PRIVATE ${SDL2_INCLUDE_DIRS})
    else()
        target_link_libraries(zepra_test SDL2::SDL2)
    endif()
endif()
if(CURL_FOUND)
    if(CURL_LIBRARIES)
        target_link_libraries(zepra_test ${CURL_LIBRARIES})
        target_include_directories(zepra_test PRIVATE ${CURL_INCLUDE_DIRS})
    else()
        target_link_libraries(zepra_test CURL::libcurl)
    endif()
endif()
target_link_libraries(zepra_test ${OPENGL_LIBRARIES} ${PLATFORM_LIBS} zepra-core webcore zepra-integration)

# Test 2: Configuration Test
add_executable(config_test config_test.cpp ${PROJECT_SOURCE_DIR}/src/config/config_manager.cpp)
target_include_directories(config_test PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)

enable_testing()
add_test(NAME ZepraBasicTest COMMAND zepra_test)

# Test 4: Browser Test Runner
add_executable(browser_test_runner browser_test_runner.cpp)
target_include_directories(browser_test_runner PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/source/webCore/include ${PROJECT_SOURCE_DIR}/source/zepraScript/include
)
target_link_libraries(browser_test_runner zepra-core webcore zepra-integration ${OPENGL_LIBRARIES} ${PLATFORM_LIBS})
add_test(NAME BrowserTestRunner COMMAND browser_test_runner)

# Test 5: CSS Cascade Test Suite
add_executable(css_cascade_test css_cascade_test.cpp)
target_include_directories(css_cascade_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/source/webCore/include
)
target_link_libraries(css_cascade_test webcore ${PLATFORM_LIBS})
add_test(NAME CSSCascadeTest COMMAND css_cascade_test)

# Test 3: Visual Test for NxSVG
if(USE_NXRENDER)
    add_executable(visual_test_nxsvg visual_test_nxsvg.cpp)
    target_link_libraries(visual_test_nxsvg nxrender ${X11_LIBRARIES} GL GLX pthread dl m Freetype::Freetype)
    target_include_directories(visual_test_nxsvg PRIVATE
        ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/source/nxrender-cpp/include
        ${X11_INCLUDE_DIR} ${FREETYPE_INCLUDE_DIRS}
    )
endif()

endif() # END Legacy test targets

# =============================================================================
# GTest-based targets
# =============================================================================
enable_testing()

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

# Test 6: nxhttp Security Tests
add_executable(nxhttp_security_test nxhttp_security_test.cpp)
target_include_directories(nxhttp_security_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/source/NXNet/nxhttp
    ${PROJECT_SOURCE_DIR}/source/NXNet/networking
)
target_link_libraries(nxhttp_security_test nxhttp networking GTest::gtest GTest::gtest_main pthread)
add_test(NAME NxHttpSecurityTest COMMAND nxhttp_security_test)

# Test 7: nxhttp URL Tests
add_executable(nxhttp_tests nxhttp_tests.cpp)
target_include_directories(nxhttp_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/source/NXNet/nxhttp
    ${PROJECT_SOURCE_DIR}/source/NXNet/networking
)
target_link_libraries(nxhttp_tests nxhttp networking GTest::gtest GTest::gtest_main pthread)
add_test(NAME NxHttpTests COMMAND nxhttp_tests)

# Test 8: nxcrypto Tests
add_executable(nxcrypto_tests nxcrypto_tests.cpp)
target_include_directories(nxcrypto_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/source/NXNet/nxcrypto
)
target_link_libraries(nxcrypto_tests nxcrypto GTest::gtest GTest::gtest_main pthread ssl crypto)
add_test(NAME NxCryptoTests COMMAND nxcrypto_tests)

# =============================================================================
# NXRender-cpp Tests
# =============================================================================
add_executable(nxrender_tests nxrender_tests.cpp)
target_include_directories(nxrender_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/source/nxrender-cpp
)
target_link_libraries(nxrender_tests
    nxrender
    GTest::gtest
    GTest::gtest_main
    pthread
    GL
    X11
    Freetype::Freetype
    z
)
add_test(NAME NxRenderTests COMMAND nxrender_tests)

# =============================================================================
# NxPdf Test Runner
# =============================================================================
add_executable(nxpdf_test nxpdf_test.cpp)
target_include_directories(nxpdf_test PRIVATE ${PROJECT_SOURCE_DIR}/source/nxrender-cpp)
target_link_libraries(nxpdf_test nxrender pthread z Freetype::Freetype)
add_test(NAME NxPdfTest COMMAND nxpdf_test)

# =============================================================================
# ZepraEngine Tests
# =============================================================================
add_executable(zepra_engine_tests
    zepra_engine_tests.cpp
    ${PROJECT_SOURCE_DIR}/source/zepraEngine/engine/html_parser_stub.cpp
)
target_include_directories(zepra_engine_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/source/zepraEngine
    ${PROJECT_SOURCE_DIR}/source/integration/include
    ${PROJECT_SOURCE_DIR}/source/integration
)
target_link_libraries(zepra_engine_tests
    GTest::gtest
    GTest::gtest_main
    pthread
)
add_test(NAME ZepraEngineTests COMMAND zepra_engine_tests)

# =============================================================================
# ZepraScript Tests
# =============================================================================
add_subdirectory(zeprascript)
