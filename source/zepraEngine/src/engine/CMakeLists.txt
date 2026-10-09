# Engine Module CMakeLists.txt
# Contains core browser engine components
# Note: webkit_engine.cpp and html_parser.cpp removed - functionality now in webCore

add_library(engine
    download_manager.cpp
    video_player.cpp
    gpu_manager.cpp
    json_bridge.cpp
    dev_tools.cpp
    webkit_engine_stub.cpp
)

# Set include directories for engine module
target_include_directories(engine 
    PUBLIC 
        ${CMAKE_SOURCE_DIR}/include/engine
        ${CMAKE_SOURCE_DIR}/include/common
        ${CMAKE_SOURCE_DIR}/include/net
)

# Set compile definitions for engine
target_compile_definitions(engine PRIVATE
    ZEPRA_ENGINE_MODULE=1
    ZEPRA_VERSION="${PROJECT_VERSION}"
)

# Set C++ standard for engine
set_target_properties(engine PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# Link dependencies for engine (if any external libs are needed)
# target_link_libraries(engine PRIVATE
#     # Add external dependencies here
# )

# Set output directory
set_target_properties(engine PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
)

# Install rules for engine
install(TARGETS engine
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/engine/
    DESTINATION include/engine
    FILES_MATCHING PATTERN "*.h"
) 