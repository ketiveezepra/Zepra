# WebCore CMakeLists.txt
# NOTE: Source files are missing - using pre-built library if available
cmake_minimum_required(VERSION 3.16)
project(WebCore VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Check if pre-built library exists
set(PREBUILT_LIB "${CMAKE_CURRENT_SOURCE_DIR}/build/libwebcore.a")

if(EXISTS ${PREBUILT_LIB})
    # Use pre-built library (source files are missing)
    message(STATUS "WebCore: Using pre-built library ${PREBUILT_LIB}")
    
    # Create interface library for includes (IMPORTED targets don't propagate includes well)
    add_library(webcore INTERFACE)
    
    # Link the pre-built static library
    target_link_libraries(webcore INTERFACE ${PREBUILT_LIB})
    
    # Set include directories - these will be propagated to consumers
    target_include_directories(webcore INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../zepraScript/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../NXNet/networking/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../NXNet/networking
        ${CMAKE_SOURCE_DIR}/source/nxrender-cpp/include
        ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/source/integration/include
    )
    
    # Find and link dependencies that the pre-built library needs
    find_package(OpenGL QUIET)
    if(OpenGL_FOUND)
        target_link_libraries(webcore INTERFACE OpenGL::GL)
        target_compile_definitions(webcore INTERFACE ZEPRA_HAS_OPENGL)
    endif()
    
    # Link with zepra-core and networking if available (these were linked when building libwebcore.a)
    if(TARGET zepra-core)
        target_link_libraries(webcore INTERFACE zepra-core)
    endif()
    if(TARGET networking)
        target_link_libraries(webcore INTERFACE networking)
    endif()
    
else()
    # Source-based build (requires source files in )
    message(STATUS "WebCore: Building from source")
    
    # Include directories
    include_directories(${CMAKE_CURRENT_SOURCE_DIR})
    
    # ZepraScript include path
    set(ZEPRASCRIPT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../zepraScript)
    include_directories(${ZEPRASCRIPT_DIR}/include)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../NXNet/networking/include)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../NXNet/networking)
    
    # Include main include directory for nxsvg.h, nxrender.h, nxfont.h
    include_directories(${CMAKE_SOURCE_DIR}/include
    include_directories(${CMAKE_SOURCE_DIR}/source/integration)
    include_directories(${CMAKE_SOURCE_DIR}/source/nxrender-cpp)
    include_directories(${CMAKE_SOURCE_DIR}/UI)
    include_directories(${CMAKE_SOURCE_DIR}/source/zepraEngine)
            ${CMAKE_SOURCE_DIR}/source/integration/include)
    
    # Source files - check which ones exist
    set(WEBCORE_SOURCES "")
    
    # Core sources
    # Find all source files in feature directories
    file(GLOB_RECURSE WEBCORE_SOURCES
        "css/*.cpp"
        "html/*.cpp"
        "browser/*.cpp"
        "scripting/*.cpp"
        "rendering/*.cpp"
        "graphics/*.cpp"
        "dom/*.cpp"
        "core/*.cpp"
    )
    
    if(WEBCORE_SOURCES)
        # Find OpenGL for GPU rendering
        find_package(OpenGL QUIET)
        
        # Library target
        add_library(webcore STATIC ${WEBCORE_SOURCES})
        
        target_include_directories(webcore PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/../zepraScript/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../NXNet/networking/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../NXNet/networking
            ${CMAKE_SOURCE_DIR}/source/nxrender-cpp/include
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/source/integration/include
        )
        
        # Link with ZepraScript (if available)
        if(TARGET zepra-core)
            target_link_libraries(webcore PUBLIC zepra-core)
            target_compile_definitions(webcore PUBLIC USE_ZEPRASCRIPT)
        endif()
        
        # Link with Networking
        if(TARGET networking)
            target_link_libraries(webcore PUBLIC networking)
        endif()
        
        if(OpenGL_FOUND)
            target_link_libraries(webcore PUBLIC OpenGL::GL)
            target_compile_definitions(webcore PUBLIC ZEPRA_HAS_OPENGL)
        endif()
        
        # Export settings
        set_target_properties(webcore PROPERTIES
            POSITION_INDEPENDENT_CODE ON
        )
    else()
        message(FATAL_ERROR "WebCore: No source files found and no pre-built library available. "
                           "Please either restore source files in  or the pre-built library.")
    endif()
endif()
