cmake_minimum_required(VERSION 3.24)

project(wujihandcpp LANGUAGES C CXX)

# Get the latest Git tag
execute_process(
    COMMAND git describe --tags --abbrev=0
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE GIT_TAG_RESULT
)

# Extract version number if tag format is valid
if(GIT_TAG_RESULT EQUAL 0 AND GIT_TAG MATCHES "^v.+$")
    string(REGEX REPLACE "^v" "" PROJECT_VERSION ${GIT_TAG})
    message(STATUS "Using Git tag version: ${PROJECT_VERSION}")
else()
    set(PROJECT_VERSION 0.0.0)
    message(STATUS "Unable to extract git tag, using version: ${PROJECT_VERSION}")
endif()

# Set C++ standard to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Set C standard to C11
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)

# Disable GNU extensions
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Add compiler options based on compiler
if(MSVC)
    add_compile_options(/W4 /Zc:preprocessor)
    add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
    # GCC/Clang
    add_compile_options(-Wall -Wextra -Wpedantic -fvisibility=hidden)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(STATIC_LINK_STDCPP "Statically link the C++ standard library" OFF)
if(MSVC)
    if(STATIC_LINK_STDCPP)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        message(STATUS "Static linking C++ standard library (MSVC)")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        message(STATUS "Dynamic linking C++ standard library (MSVC)")
    endif()
endif()

# Get project sources
file(GLOB_RECURSE PROJECT_SOURCE CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/src/*.cpp
    ${PROJECT_SOURCE_DIR}/src/*.c
)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Tactile-board CDC stack is Linux-only: cdc_byte_stream.cpp uses
    # termios + poll(2) + /sys/class/tty (the latter via
    # cdc_transport.cpp::discover_devices), and tactile_board.cpp /
    # frame_demuxer.cpp build on top of those Linux-specific kernel
    # interfaces. The public tactile headers are gated on
    # `#if defined(__linux__)` so non-Linux consumers also see them as
    # empty — keeping the exclusion list and the header gates aligned
    # is what stops `find_package(wujihandcpp)` consumers from
    # hitting an unresolved-symbol link error on macOS / Windows.
    list(REMOVE_ITEM PROJECT_SOURCE
        ${PROJECT_SOURCE_DIR}/src/device/tactile_board.cpp
        ${PROJECT_SOURCE_DIR}/src/device/frame_demuxer.cpp
        ${PROJECT_SOURCE_DIR}/src/transport/cdc_byte_stream.cpp
        ${PROJECT_SOURCE_DIR}/src/transport/cdc_transport.cpp
    )
endif()

option(BUILD_STATIC_WUJIHANDCPP "Build static library wujihandcpp" OFF)
if(BUILD_STATIC_WUJIHANDCPP)
    add_library(
        ${PROJECT_NAME} STATIC
        ${PROJECT_SOURCE}
    )
    target_compile_definitions(${PROJECT_NAME} PUBLIC STATIC_LINKING_WUJIHANDCPP)
    message(STATUS "Building static library wujihandcpp")
else()
    add_library(
        ${PROJECT_NAME} SHARED
        ${PROJECT_SOURCE}
    )
    message(STATUS "Building shared library wujihandcpp")
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE BUILDING_WUJIHANDCPP)
target_compile_definitions(${PROJECT_NAME} PUBLIC WUJIHANDCPP_VERSION="${PROJECT_VERSION}")

# Public include split between BUILD and INSTALL interfaces so the install
# tree (lib/cmake/wujihandcpp/wujihandcppTargets.cmake) records `include`
# under the install prefix instead of leaking the source-tree path.
target_include_directories(${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/src)

if(WIN32)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
    target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${LIBUSB_INCLUDE_DIRS})
    target_link_directories(${PROJECT_NAME} PUBLIC "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
    target_link_libraries(${PROJECT_NAME} PUBLIC ${LIBUSB_LIBRARIES})
elseif(UNIX)
    target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE /usr/include/libusb-1.0)
    target_link_libraries(${PROJECT_NAME} PUBLIC usb-1.0)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} PUBLIC Threads::Threads)
endif()

# Install rules are gated so embedding builds (e.g. the wujihandpy Python
# wheel, which embeds wujihandcpp into _core.so via add_subdirectory) can
# skip them. Standalone builds (and the system .deb/.rpm packaging path)
# leave it ON to produce a fully consumable install tree. Declared here —
# before spdlog FetchContent — so SPDLOG_INSTALL can mirror it.
option(WUJIHANDCPP_INSTALL "Generate install rules for wujihandcpp" ON)

include(FetchContent)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.16.0
    GIT_SHALLOW TRUE
)
# Ship spdlog alongside wujihandcpp when we are installing. Downstream
# consumers do `find_package(wujihandcpp CONFIG REQUIRED)` which transitively
# `find_dependency(spdlog)` — that has to resolve, and the simplest place to
# resolve it is the same prefix wujihandcpp installed into. When wheel /
# embedding builds set WUJIHANDCPP_INSTALL=OFF, spdlog stays out of the
# install set too.
if(WUJIHANDCPP_INSTALL)
    set(SPDLOG_INSTALL ON CACHE BOOL "" FORCE)
else()
    set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
endif()
FetchContent_MakeAvailable(spdlog)
target_link_libraries(${PROJECT_NAME} PUBLIC spdlog::spdlog)

if(NOT MSVC)
    # GCC/Clang
    if(STATIC_LINK_STDCPP)
        target_compile_options(${PROJECT_NAME} PRIVATE
            -fdata-sections
            -ffunction-sections
        )
        target_link_options(${PROJECT_NAME} PRIVATE -static-libstdc++ -static-libgcc)
        target_link_options(${PROJECT_NAME} PRIVATE -Wl,--exclude-libs,ALL -Wl,--gc-sections)
        message(STATUS "Static linking C++ standard library")
    else()
        message(STATUS "Dynamic linking C++ standard library")
    endif()
endif()

# LTO is opt-in. Enabling it bakes the host compiler's LTO bytecode into the
# static archive, which then refuses to link with a different-major-version
# GCC downstream (we hit this when the SDK was built with GCC 13 and the ROS
# driver was built with the system GCC 11). Default OFF keeps source-link
# downstream consumers — like the wujihandros2 driver — portable; release
# builds that control the whole toolchain can flip it on.
option(WUJIHANDCPP_ENABLE_IPO "Enable interprocedural optimization (LTO)" OFF)
if(WUJIHANDCPP_ENABLE_IPO)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION TRUE
    )
endif()

if(WUJIHANDCPP_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS ${PROJECT_NAME}
        EXPORT wujihandcppTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT wujihandcppTargets
        FILE wujihandcppTargets.cmake
        NAMESPACE wujihandcpp::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wujihandcpp
    )

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/wujihandcppConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wujihandcppConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/wujihandcppConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wujihandcpp
    )

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/wujihandcppConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/wujihandcppConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wujihandcpp
    )

    # Also export the build tree so consumers can use the SDK without an
    # install step:
    #   find_package(wujihandcpp PATHS .../wujihandcpp/build NO_DEFAULT_PATH)
    # Useful for the local dev loop where SDK and ROS sit side by side.
    # configure_package_config_file already wrote Config + ConfigVersion to
    # this binary dir; export(EXPORT) writes the matching Targets file.
    export(EXPORT wujihandcppTargets
        FILE "${CMAKE_CURRENT_BINARY_DIR}/wujihandcppTargets.cmake"
        NAMESPACE wujihandcpp::
    )
endif()

include(CTest)
if(BUILD_TESTING)
    include(FetchContent)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)

    file(GLOB_RECURSE WUJIHANDCPP_TEST_SOURCES CONFIGURE_DEPENDS
        ${PROJECT_SOURCE_DIR}/tests/*.cpp
    )
    # Exclude tests/cpp11_compat/main.cpp from this gtest target — it has
    # its own int main() and is built as a separate standalone executable
    # by tests/cpp11_compat/CMakeLists.txt. Without this filter, its main
    # symbol collides with gtest_main and the gtest binary silently exits
    # with the cpp11 probe's success message instead of running any tests.
    list(FILTER WUJIHANDCPP_TEST_SOURCES EXCLUDE REGEX "/cpp11_compat/")
    # Tactile tests link against frame_demuxer + tactile_board + cdc_byte_stream,
    # all Linux-only (matching the .cpp exclusion above and the public-header
    # `#if defined(__linux__)` gate). Without this filter, BUILD_TESTING=ON on
    # macOS / Windows still globs frame_demuxer_test.cpp and the test target
    # fails to compile because `wujihandcpp::tactile::*` is invisible on
    # those platforms.
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(FILTER WUJIHANDCPP_TEST_SOURCES EXCLUDE REGEX "/tests/device/frame_demuxer_test\\.cpp$")
    endif()

    add_executable(wujihandcpp_tests
        ${WUJIHANDCPP_TEST_SOURCES}
    )
    # Tests need access to private src/ headers (FrameDemuxer + IByteStream
    # are implementation details, not part of the public API surface). The
    # main lib only exposes include/ to consumers via PUBLIC; we explicitly
    # widen the test target's include path here.
    target_include_directories(wujihandcpp_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(wujihandcpp_tests PRIVATE gtest_main ${PROJECT_NAME})

    add_test(NAME wujihandcpp_tests COMMAND wujihandcpp_tests)
endif()

if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB;RPM")

    set(ARCHITECTURE ${CMAKE_SYSTEM_PROCESSOR})
    if(ARCHITECTURE STREQUAL "x86_64")
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
        set(CPACK_RPM_PACKAGE_ARCHITECTURE "x86_64")
    elseif(ARCHITECTURE STREQUAL "aarch64")
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
        set(CPACK_RPM_PACKAGE_ARCHITECTURE "aarch64")
    else()
        message(FATAL_ERROR "Unsupported architecture: ${ARCHITECTURE}.")
    endif()

    set(CPACK_DEBIAN_PACKAGE_NAME "${PROJECT_NAME}")
    set(CPACK_DEBIAN_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Wuji Technology <support@pan-motor.com>")
    set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "A Lightweight C++ SDK for Wujihand")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/wuji-technology/wujihandcpp")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libusb-1.0-0-dev")
    set(CPACK_DEBIAN_PACKAGE_SECTION "libs")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")

    set(CPACK_RPM_PACKAGE_NAME "${PROJECT_NAME}")
    set(CPACK_RPM_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_RPM_PACKAGE_VENDOR "Wuji Technology <support@pan-motor.com>")
    set(CPACK_RPM_PACKAGE_DESCRIPTION "A Lightweight C++ SDK for Wujihand")
    set(CPACK_RPM_PACKAGE_URL "https://github.com/wuji-technology/wujihandcpp")
    set(CPACK_RPM_PACKAGE_RELEASE "1")
    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_REQUIRES "libusbx-devel")
    set(CPACK_RPM_PACKAGE_GROUP "Development/Libraries")

    set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}-${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

    include(CPack)
endif()
