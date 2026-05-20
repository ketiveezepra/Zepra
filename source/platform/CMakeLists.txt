cmake_minimum_required(VERSION 3.16)
project(Platform VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Common sources
set(PLATFORM_SOURCES
    src/event_loop.cpp
    src/clipboard.cpp
)

# Platform-specific sources
if(UNIX AND NOT APPLE)
    list(APPEND PLATFORM_SOURCES
        src/linux/window_system_linux.cpp
    )
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(X11 QUIET x11)
endif()

if(WIN32)
    list(APPEND PLATFORM_SOURCES
        src/windows/window_system_win.cpp
    )
endif()

if(APPLE)
    list(APPEND PLATFORM_SOURCES
        src/macos/window_system_macos.mm
    )
endif()

add_library(platform STATIC ${PLATFORM_SOURCES})

target_include_directories(platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(X11_FOUND)
    target_link_libraries(platform ${X11_LIBRARIES})
    target_include_directories(platform PRIVATE ${X11_INCLUDE_DIRS})
endif()

if(WIN32)
    target_link_libraries(platform user32 gdi32 opengl32)
    target_compile_definitions(platform PRIVATE
        WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS ZEPRA_PLATFORM_WINDOWS)
endif()

set_target_properties(platform PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
