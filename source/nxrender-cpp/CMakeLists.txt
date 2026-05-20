cmake_minimum_required(VERSION 3.28) # Dont change this
project(nxrender VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
set(NXRENDER_SOURCES
    # nxgfx — GPU drawing backend
    nxgfx/color.cpp
    nxgfx/context.cpp
    nxgfx/context3d.cpp
    nxgfx/text.cpp
    nxgfx/text/text_shaper.cpp
    nxgfx/text/font_fallback.cpp
    nxgfx/text/glyph_cache.cpp
    nxgfx/math/matrix.cpp
    nxgfx/math/quaternion.cpp
    nxgfx/math/transform2d.cpp
    nxgfx/math/transform3d.cpp
    nxgfx/path/path.cpp
    nxgfx/path/path_builder.cpp
    nxgfx/path/tessellator.cpp
    nxgfx/path/stroke.cpp
    nxgfx/shader.cpp
    nxgfx/batch_renderer.cpp
    nxgfx/texture_atlas.cpp
    nxgfx/image_decoder.cpp
    nxgfx/gradient.cpp
    nxgfx/filter_pipeline.cpp
    nxgfx/render_target.cpp
    nxgfx/clip_stack.cpp
    nxgfx/text/text_layout.cpp
    nxgfx/svg/svg_renderer.cpp
    nxgfx/transform.cpp

    # core — rendering pipeline
    core/compositor.cpp
    core/damage_tracker.cpp
    core/layer_tree_host.cpp
    core/render_tile.cpp
    core/tile_manager.cpp
    core/surface.cpp
    core/display_list.cpp
    core/resource_cache.cpp
    core/frame_scheduler.cpp
    core/render_pipeline.cpp

    # widgets — UI components
    widgets/widget.cpp
    widgets/datagrid.cpp
    widgets/button.cpp
    widgets/label.cpp
    widgets/textfield.cpp
    widgets/container.cpp
    widgets/scrollview.cpp
    widgets/listview.cpp
    widgets/tabview.cpp
    widgets/tree_view.cpp
    widgets/menu.cpp
    widgets/docking_manager.cpp
    widgets/progress_bar.cpp
    widgets/tooltip.cpp
    widgets/dialog.cpp
    widgets/splitter.cpp
    widgets/checkbox.cpp
    widgets/slider.cpp
    widgets/dropdown.cpp

    # layout — CSS-grade layout engine
    layout/flexbox.cpp
    layout/block_layout.cpp
    layout/inline_layout.cpp
    layout/positioned_layout.cpp
    layout/grid.cpp
    layout/table_layout.cpp
    layout/stacking_context.cpp
    layout/multi_column.cpp
    layout/container_query.cpp

    # theme — design tokens and widget styles
    theme/theme.cpp
    theme/style_sheet.cpp
    theme/design_tokens.cpp

    # input — event handling
    input/mouse.cpp
    input/keyboard.cpp
    input/event_router.cpp
    input/focus_manager.cpp
    input/shortcut_manager.cpp
    input/gesture_recognizer.cpp
    input/clipboard.cpp

    # animation — keyframes, transitions, interpolators
    animation/animator.cpp
    animation/keyframe.cpp
    animation/transition.cpp
    animation/scroll_physics.cpp
    animation/spring.cpp
    animation/css_animation.cpp

    # platform — OS abstraction
    platform/linux_x11.cpp
    platform/display_info.cpp
    platform/platform_timer.cpp
    platform/thread_pool.cpp
    platform/linux_wayland.cpp

    # accessibility
    accessibility/accessible.cpp
    accessibility/accessibility_tree.cpp
    accessibility/a11y_bridge.cpp

    # web layer
    web/dom_renderer.cpp
    web/style_resolver.cpp
    web/event_router.cpp
    web/css/property_ids.cpp
    web/css/value_parser.cpp
    web/css/cascade.cpp
    web/css/selector.cpp
    web/css/custom_properties.cpp
    web/css/css_math.cpp
    web/css/typed_om.cpp
    web/box/box_tree.cpp
    web/paint/paint_ops.cpp
    web/paint/painters.cpp
    web/paint/hit_test.cpp
    web/paint/image_loader.cpp
    web/observers.cpp
    web/containment.cpp
    web/resource_loader.cpp
    web/frame_orchestrator.cpp
    web/animation.cpp
    web/events.cpp
    web/input.cpp
    web/document_view.cpp
    web/forms.cpp
    web/accessibility.cpp
    web/advanced.cpp
    web/compositor.cpp
    web/table_and_media.cpp
    web/service_and_selection.cpp

    # canvas 2D & WebGL
    nxgfx/canvas2d/canvas_context.cpp
    nxgfx/webgl/webgl_context.cpp

    # form control widgets
    widgets/form_controls.cpp

    # media pipeline
    media/media_element.cpp
    media/media_pipeline.cpp

    # scroll snap + advanced layout
    layout/scroll_snap.cpp
    layout/advanced_layout.cpp

    # bridge to libnxrender.a (C library) - DISABLED: requires external nxgfx C lib
    # nxrender_c_bridge.cpp

    # main
    nxrender.cpp
)

# PDF Backend Configuration
option(NXPDF_USE_BUILTIN "Use built-in nxpdf library (ships with Zepra)" ON)
option(NXPDF_USE_OS "Use NeolyxOS syscall/IPC for PDF rendering" OFF)

if(NXPDF_USE_BUILTIN)
    list(APPEND NXRENDER_SOURCES
        nxpdf/nx_pdf.cpp
        nxpdf/parser/nx_pdf_lexer.cpp
        nxpdf/parser/nx_pdf_xref.cpp
        nxpdf/parser/nx_pdf_objects.cpp
        nxpdf/parser/nx_pdf_document.cpp
        nxpdf/renderer/nx_pdf_page.cpp
        nxpdf/renderer/nx_pdf_content.cpp
        nxpdf/renderer/nx_pdf_graphics.cpp
        nxpdf/renderer/nx_pdf_text.cpp
        nxpdf/fonts/nx_pdf_font.cpp
        nxpdf/fonts/nx_pdf_tounicode.cpp
        nxpdf/images/nx_pdf_image.cpp
        nxpdf/editor/nx_pdf_edit.cpp
        nxpdf/editor/nx_pdf_annot.cpp
        nxpdf/editor/nx_pdf_writer.cpp
    )
endif()

# Create library
add_library(nxrender STATIC ${NXRENDER_SOURCES})

# Include directories
target_include_directories(nxrender PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/nxpdf
)

if(NXPDF_USE_BUILTIN)
    target_compile_definitions(nxrender PUBLIC NXPDF_USE_BUILTIN)
elseif(NXPDF_USE_OS)
    target_compile_definitions(nxrender PUBLIC NXPDF_USE_OS)
endif()

# Include C library headers for bridge (from nxrender_c)
target_include_directories(nxrender PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../gui/nxrender_c/include
)

# Link dependencies
find_package(OpenGL REQUIRED)
find_package(Freetype REQUIRED)

# HarfBuzz: use pkg-config when available (Linux), fall back to find_library (Windows)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(HARFBUZZ REQUIRED harfbuzz)
    target_include_directories(nxrender PRIVATE ${HARFBUZZ_INCLUDE_DIRS})
    set(HARFBUZZ_LINK_LIBS ${HARFBUZZ_LIBRARIES})
else()
    # Windows / systems without pkg-config
    find_library(HARFBUZZ_LIB NAMES harfbuzz)
    find_path(HARFBUZZ_INCLUDE_DIR hb.h PATH_SUFFIXES harfbuzz)
    if(HARFBUZZ_LIB AND HARFBUZZ_INCLUDE_DIR)
        target_include_directories(nxrender PRIVATE ${HARFBUZZ_INCLUDE_DIR})
        set(HARFBUZZ_LINK_LIBS ${HARFBUZZ_LIB})
        message(STATUS "Found HarfBuzz: ${HARFBUZZ_LIB}")
    else()
        message(WARNING "HarfBuzz not found — text shaping will be disabled. Install harfbuzz-devel or provide it in CMAKE_PREFIX_PATH.")
        set(HARFBUZZ_LINK_LIBS "")
    endif()
endif()

target_link_libraries(nxrender PUBLIC
    OpenGL::GL
    Freetype::Freetype
    ${HARFBUZZ_LINK_LIBS}
)

# Platform-specific
if(UNIX AND NOT APPLE)
    target_link_libraries(nxrender PUBLIC X11 Xrandr pthread)
endif()

if(WIN32)
    # Windows platform source is gated to _WIN32 inside the file itself,
    # but we add it here so CMake includes it in the build.
    target_sources(nxrender PRIVATE platform/windows_wgl.cpp)
    target_link_libraries(nxrender PUBLIC opengl32 gdi32 user32)
    target_compile_definitions(nxrender PUBLIC ZEPRA_PLATFORM_WINDOWS)
    # Suppress MSVC/MinGW POSIX deprecation noise
    target_compile_definitions(nxrender PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
endif()

# Install
install(TARGETS nxrender
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY accessibility animation core input layout nxgfx platform theme web widgets
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# Examples (optional)
option(BUILD_EXAMPLES "Build example applications" OFF)
if(BUILD_EXAMPLES)
    add_executable(browser_ui_example examples/browser_ui_example.cpp)
    target_link_libraries(browser_ui_example PRIVATE nxrender)
endif()
