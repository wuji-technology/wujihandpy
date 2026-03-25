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

# --- wujihandcpp ---
# Prefer system-installed wujihandcpp (apt package) to avoid GCC <format> issues.
# Fall back to add_subdirectory for source builds with GCC 13+.
find_library(WUJIHANDCPP_LIB wujihandcpp)
find_path(WUJIHANDCPP_INCLUDE wujihandcpp/device/hand.hpp)
if(WUJIHANDCPP_LIB AND WUJIHANDCPP_INCLUDE)
    message(STATUS "Using system wujihandcpp: ${WUJIHANDCPP_LIB}")
    add_library(wujihandcpp SHARED IMPORTED)
    set_target_properties(wujihandcpp PROPERTIES
        IMPORTED_LOCATION "${WUJIHANDCPP_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${WUJIHANDCPP_INCLUDE}"
    )
    # wujihandcpp depends on spdlog, libusb, threads
    find_package(Threads REQUIRED)
    find_package(spdlog QUIET)
    if(spdlog_FOUND)
        target_link_libraries(wujihandcpp INTERFACE spdlog::spdlog Threads::Threads usb-1.0)
    else()
        # spdlog bundled in system lib, just link transitive deps
        target_link_libraries(wujihandcpp INTERFACE Threads::Threads usb-1.0)
    endif()
else()
    message(STATUS "System wujihandcpp not found, building from source")
    set(BUILD_STATIC_WUJIHANDCPP ON CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    add_subdirectory(../../wujihandcpp ${CMAKE_CURRENT_BINARY_DIR}/wujihandcpp)
endif()

# --- FetchContent dependencies ---
include(FetchContent)

# Disable zenoh-cpp extras
set(ZENOHCXX_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ZENOHCXX_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZENOHCXX_EXAMPLES_PROTOBUF OFF CACHE BOOL "" FORCE)
# Disable nlohmann_json extras
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)

# Pin released Zenoh versions so bridge builds stay reproducible.
FetchContent_Declare(
    zenohc
    GIT_REPOSITORY https://github.com/eclipse-zenoh/zenoh-c.git
    GIT_TAG 1.8.0
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    zenohcxx
    GIT_REPOSITORY https://github.com/eclipse-zenoh/zenoh-cpp.git
    GIT_TAG 1.8.0
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(zenohc zenohcxx nlohmann_json)

# --- Bridge executable ---
add_executable(${PROJECT_NAME}
    src/main.cpp
    src/hand_bridge.cpp
)
target_link_libraries(${PROJECT_NAME} PRIVATE
    wujihandcpp
    zenohcxx::zenohc
    nlohmann_json::nlohmann_json
)
