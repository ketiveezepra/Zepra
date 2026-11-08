
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
target_link_libraries(nxhttp_security_test nxhttp networking GTest::gtest GTest::gtest_main)
if(NOT WIN32)
    target_link_libraries(nxhttp_security_test pthread)
endif()
add_test(NAME NxHttpSecurityTest COMMAND nxhttp_security_test)

# Test 7: nxhttp URL Tests
add_executable(nxhttp_tests nxhttp_tests.cpp)
target_include_directories(nxhttp_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/source/NXNet/nxhttp
    ${PROJECT_SOURCE_DIR}/source/NXNet/networking
)
target_link_libraries(nxhttp_tests nxhttp networking GTest::gtest GTest::gtest_main)
if(NOT WIN32)
    target_link_libraries(nxhttp_tests pthread)
endif()
add_test(NAME NxHttpTests COMMAND nxhttp_tests)

# Test 8: nxcrypto Tests
add_executable(nxcrypto_tests nxcrypto_tests.cpp)
target_include_directories(nxcrypto_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/source/NXNet/nxcrypto
)
target_link_libraries(nxcrypto_tests nxcrypto GTest::gtest GTest::gtest_main)
if(WIN32)
    target_link_libraries(nxcrypto_tests ${OPENSSL_LIBRARIES})
elseif(APPLE)
    # Homebrew OpenSSL is not on the default linker path; use the found paths
    target_link_libraries(nxcrypto_tests ${OPENSSL_LIBRARIES})
else()
    target_link_libraries(nxcrypto_tests pthread ssl crypto)
endif()
add_test(NAME NxCryptoTests COMMAND nxcrypto_tests)

# =============================================================================
# NXRender-cpp Tests
# =============================================================================
add_executable(nxrender_tests nxrender_tests.cpp canvas2d_tests.cpp)
target_include_directories(nxrender_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/source/nxrender-cpp
)
target_link_libraries(nxrender_tests
    nxrender
    GTest::gtest
    GTest::gtest_main
    Freetype::Freetype
    z
)
if(WIN32)
    target_link_libraries(nxrender_tests opengl32 gdi32)
elseif(APPLE)
    # Cocoa/OpenGL frameworks come with nxrender
    target_link_libraries(nxrender_tests pthread)
else()
    target_link_libraries(nxrender_tests GL X11 pthread)
endif()
add_test(NAME NxRenderTests COMMAND nxrender_tests)

# =============================================================================
# NxPdf Test Runner
# =============================================================================
add_executable(nxpdf_test nxpdf_test.cpp)
target_include_directories(nxpdf_test PRIVATE ${PROJECT_SOURCE_DIR}/source/nxrender-cpp)
target_link_libraries(nxpdf_test nxrender z Freetype::Freetype)
if(NOT WIN32)
    target_link_libraries(nxpdf_test pthread)
endif()
add_test(NAME NxPdfTest COMMAND nxpdf_test
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/hello.pdf "Hello NXRender")

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
)
if(NOT WIN32)
    target_link_libraries(zepra_engine_tests pthread)
endif()
add_test(NAME ZepraEngineTests COMMAND zepra_engine_tests)

# =============================================================================
# WebCore layout tests (default styles, presentational hints, tables)
# =============================================================================
if(TARGET webcore)
    add_executable(webcore_layout_tests webcore_layout_tests.cpp)
    target_link_libraries(webcore_layout_tests webcore GTest::gtest GTest::gtest_main)
    if(NOT WIN32)
        target_link_libraries(webcore_layout_tests pthread)
    endif()
    add_test(NAME WebCoreLayoutTests COMMAND webcore_layout_tests)

    # Page scripts and the DOM: live wrappers, mutations, events
    add_executable(webcore_dom_bindings_tests webcore_dom_bindings_tests.cpp)
    target_link_libraries(webcore_dom_bindings_tests webcore GTest::gtest GTest::gtest_main)
    if(NOT WIN32)
        target_link_libraries(webcore_dom_bindings_tests pthread)
    endif()
    add_test(NAME WebCoreDomBindingsTests COMMAND webcore_dom_bindings_tests)

    # Media queries: parsing and matching, @media rules as the viewport changes, matchMedia()
    add_executable(webcore_media_query_tests webcore_media_query_tests.cpp)
    target_link_libraries(webcore_media_query_tests webcore GTest::gtest GTest::gtest_main)
    if(NOT WIN32)
        target_link_libraries(webcore_media_query_tests pthread)
    endif()
    add_test(NAME WebCoreMediaQueryTests COMMAND webcore_media_query_tests)

    # :hover / :active / :focus forced by DevTools, alongside the real interaction state
    add_executable(webcore_forced_pseudo_tests webcore_forced_pseudo_tests.cpp)
    target_link_libraries(webcore_forced_pseudo_tests webcore GTest::gtest GTest::gtest_main)
    if(NOT WIN32)
        target_link_libraries(webcore_forced_pseudo_tests pthread)
    endif()
    add_test(NAME WebCoreForcedPseudoTests COMMAND webcore_forced_pseudo_tests)

    # CSS @import: parsing, loading through the engine's fetcher, media queries, cascade order
    add_executable(webcore_css_import_tests webcore_css_import_tests.cpp)
    target_link_libraries(webcore_css_import_tests webcore GTest::gtest GTest::gtest_main)
    if(NOT WIN32)
        target_link_libraries(webcore_css_import_tests pthread)
    endif()
    add_test(NAME WebCoreCssImportTests COMMAND webcore_css_import_tests)

    # Keys, typing, focus and forms: control values, key/input/focus/change/submit
    # events, click activation, and the browser's platform-key to DOM-key mapping
    add_executable(webcore_user_input_tests webcore_user_input_tests.cpp
                   ${PROJECT_SOURCE_DIR}/source/integration/page_keys.cpp
                   ${PROJECT_SOURCE_DIR}/source/integration/select_popup.cpp)
    target_include_directories(webcore_user_input_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/source/integration ${PROJECT_SOURCE_DIR}/source/nxrender-cpp)
    target_link_libraries(webcore_user_input_tests webcore GTest::gtest GTest::gtest_main)
    if(NOT WIN32)
        target_link_libraries(webcore_user_input_tests pthread)
    endif()
    add_test(NAME WebCoreUserInputTests COMMAND webcore_user_input_tests)

    # The DevTools Console: value previews, console methods, console helpers
    add_executable(webcore_console_format_tests webcore_console_format_tests.cpp)
    target_link_libraries(webcore_console_format_tests webcore GTest::gtest GTest::gtest_main)
    if(NOT WIN32)
        target_link_libraries(webcore_console_format_tests pthread)
    endif()
    add_test(NAME WebCoreConsoleFormatTests COMMAND webcore_console_format_tests)

    # The DevTools Rules pane: matched rules, cascade winners, inherited rules, switched-off declarations
    add_executable(devtools_style_rules_tests devtools_style_rules_tests.cpp
                   ${PROJECT_SOURCE_DIR}/source/integration/devtools/style_rules.cpp)
    target_include_directories(devtools_style_rules_tests PRIVATE ${PROJECT_SOURCE_DIR}/source/integration)
    target_link_libraries(devtools_style_rules_tests webcore GTest::gtest GTest::gtest_main)
    if(NOT WIN32)
        target_link_libraries(devtools_style_rules_tests pthread)
    endif()
    add_test(NAME DevToolsStyleRulesTests COMMAND devtools_style_rules_tests)

    # The DevTools text field: caret, selection and editing keys over UTF-8 text
    add_executable(devtools_text_field_tests devtools_text_field_tests.cpp
                   ${PROJECT_SOURCE_DIR}/source/integration/devtools/text_field.cpp)
    target_include_directories(devtools_text_field_tests PRIVATE
                               ${PROJECT_SOURCE_DIR}/source/integration
                               ${PROJECT_SOURCE_DIR}/source/nxrender-cpp)
    target_link_libraries(devtools_text_field_tests GTest::gtest GTest::gtest_main)
    if(NOT WIN32)
        target_link_libraries(devtools_text_field_tests pthread)
    endif()
    add_test(NAME DevToolsTextFieldTests COMMAND devtools_text_field_tests)

    # The DevTools markup view: attribute, text and HTML edits with undo, delete, duplicate,
    # Copy CSS Selector, Copy outerHTML, markup search, and the lifetime of removed nodes
    add_executable(devtools_dom_edit_tests devtools_dom_edit_tests.cpp
                   ${PROJECT_SOURCE_DIR}/source/integration/devtools/dom_edit.cpp)
    target_include_directories(devtools_dom_edit_tests PRIVATE ${PROJECT_SOURCE_DIR}/source/integration)
    target_link_libraries(devtools_dom_edit_tests webcore GTest::gtest GTest::gtest_main)
    if(NOT WIN32)
        target_link_libraries(devtools_dom_edit_tests pthread)
    endif()
    add_test(NAME DevToolsDomEditTests COMMAND devtools_dom_edit_tests)

    # window.getComputedStyle(): resolved values from the CSS engine, read-only declarations
    add_executable(computed_style_tests computed_style_tests.cpp)
    target_link_libraries(computed_style_tests webcore GTest::gtest GTest::gtest_main)
    if(NOT WIN32)
        target_link_libraries(computed_style_tests pthread)
    endif()
    add_test(NAME ComputedStyleTests COMMAND computed_style_tests)

    # Layout geometry for page scripts: element boxes, getBoundingClientRect, offset*, client*, scrolling
    add_executable(layout_geometry_tests layout_geometry_tests.cpp
                   ${PROJECT_SOURCE_DIR}/source/webCore/browser/layout_geometry.cpp)
    target_link_libraries(layout_geometry_tests webcore GTest::gtest GTest::gtest_main)
    if(NOT WIN32)
        target_link_libraries(layout_geometry_tests pthread)
    endif()
    add_test(NAME LayoutGeometryTests COMMAND layout_geometry_tests)
    # The DevTools Network panel: filter terms, Headers, Response lines, Timings, text bodies kept
    add_executable(devtools_network_tests devtools_network_tests.cpp
                   ${PROJECT_SOURCE_DIR}/source/integration/devtools/network_details.cpp)
    target_include_directories(devtools_network_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/source/integration
        ${PROJECT_SOURCE_DIR}/source/webCore)
    target_link_libraries(devtools_network_tests networking GTest::gtest GTest::gtest_main)
    if(NOT WIN32)
        target_link_libraries(devtools_network_tests pthread)
    endif()
    add_test(NAME DevToolsNetworkTests COMMAND devtools_network_tests)

    # The DevTools Console input: editor, unfinished input, autocomplete query and ranking, output filter
    add_executable(devtools_console_input_tests devtools_console_input_tests.cpp
                   ${PROJECT_SOURCE_DIR}/source/integration/devtools/console_input.cpp)
    target_include_directories(devtools_console_input_tests PRIVATE ${PROJECT_SOURCE_DIR}/source/integration)
    target_link_libraries(devtools_console_input_tests webcore GTest::gtest GTest::gtest_main)
    if(NOT WIN32)
        target_link_libraries(devtools_console_input_tests pthread)
    endif()
    add_test(NAME DevToolsConsoleInputTests COMMAND devtools_console_input_tests)

    # Hostile input for the HTML, CSS and JavaScript parsers (hangs, stack overflows)
    add_executable(parser_robustness_tests parser_robustness_tests.cpp)
    target_link_libraries(parser_robustness_tests webcore GTest::gtest GTest::gtest_main)
    if(NOT WIN32)
        target_link_libraries(parser_robustness_tests pthread)
    endif()
    add_test(NAME ParserRobustnessTests COMMAND parser_robustness_tests)
    # A regression here is a hang: fail instead of stalling the CI job
    set_tests_properties(ParserRobustnessTests PROPERTIES TIMEOUT 120)

    # SVG rasterizing (nanosvg + rewrite pass) for <img>, inline <svg> and icons
    add_executable(svg_raster_tests svg_raster_tests.cpp)
    target_link_libraries(svg_raster_tests webcore GTest::gtest GTest::gtest_main)
    add_test(NAME SvgRasterTests COMMAND svg_raster_tests)
endif()

# =============================================================================
# ZepraScript Tests
# =============================================================================
add_subdirectory(zeprascript)
