cmake_minimum_required(VERSION 3.24)

project(wujihand_zenoh_bridge LANGUAGES C CXX)

# C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default build type: RelWithDebInfo
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Compiler warnings
if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# --- wujihandcpp (static) ---
set(BUILD_STATIC_WUJIHANDCPP ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(../../wujihandcpp ${CMAKE_CURRENT_BINARY_DIR}/wujihandcpp)

# --- FetchContent dependencies ---
include(FetchContent)

# Disable zenoh-cpp extras
set(ZENOHCXX_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ZENOHCXX_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZENOHCXX_EXAMPLES_PROTOBUF OFF CACHE BOOL "" FORCE)
# Disable nlohmann_json extras
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    zenohc
    GIT_REPOSITORY https://github.com/eclipse-zenoh/zenoh-c.git
    GIT_TAG main
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    zenohcxx
    GIT_REPOSITORY https://github.com/eclipse-zenoh/zenoh-cpp.git
    GIT_TAG main
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(zenohc zenohcxx nlohmann_json)

# --- Sources ---
file(GLOB PROJECT_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

# --- Target ---
add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

target_link_libraries(${PROJECT_NAME} PRIVATE
    wujihandcpp
    zenohcxx::zenohc
    nlohmann_json::nlohmann_json
)
