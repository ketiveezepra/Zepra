cmake_minimum_required(VERSION 3.16)
project(Storage VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Source files
set(STORAGE_SOURCES
    src/local_storage.cpp
    src/session_storage.cpp
    src/permission_manager.cpp
    src/quota_manager.cpp
    src/cache_storage.cpp
    src/site_settings.cpp
    src/privacy_settings_ui.cpp
)

# Library target
add_library(storage STATIC ${STORAGE_SOURCES})

target_include_directories(storage PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../NXNet/networking/include
)

# Export settings
set_target_properties(storage PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
