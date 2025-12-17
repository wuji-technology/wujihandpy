cmake_minimum_required(VERSION 3.15)

if(DEFINED pybind11_ROOT AND pybind11_ROOT STREQUAL "AUTO_DETECT")
    execute_process(
        COMMAND "python" -m pybind11 --cmakedir
        RESULT_VARIABLE _pybind11_rc
        OUTPUT_VARIABLE _pybind11_cmake_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    if(_pybind11_rc EQUAL 0 AND IS_DIRECTORY "${_pybind11_cmake_dir}")
        set(pybind11_ROOT "${_pybind11_cmake_dir}"
            CACHE PATH "pybind11 root (auto-detected from 'python -m pybind11 --cmakedir')" FORCE)
    else()
        message(FATAL_ERROR
            "AUTO_DETECT for pybind11_ROOT failed: "
            "return code=${_pybind11_rc}, stdout='${_pybind11_cmake_dir}'. "
            "Set pybind11_ROOT manually.")
    endif()
endif()

if(WIN32)
    if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        if(DEFINED ENV{VCPKG_ROOT})
            set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "" FORCE)
        endif()
    endif()
endif()

project(wujihandpy)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# TPDO_SCOPE_C12 debug feature (disabled by default)
option(WUJI_SCOPE_DEBUG "Enable TPDO_SCOPE_C12 debug feature" OFF)

set(BUILD_STATIC_WUJIHANDCPP ON CACHE BOOL "" FORCE)
add_subdirectory(wujihandcpp EXCLUDE_FROM_ALL)

# Apply WUJI_SCOPE_DEBUG to both wujihandcpp and _core targets
if(WUJI_SCOPE_DEBUG)
    target_compile_definitions(wujihandcpp PRIVATE WUJI_SCOPE_DEBUG)
    message(STATUS "WUJI_SCOPE_DEBUG enabled")
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

# Set default build type to Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Add compiler options based on compiler
if(MSVC)
    add_compile_options(/W4 /Zc:preprocessor /EHsc)
    add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
else()
    # GCC/Clang
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Get project sources
file(GLOB_RECURSE PROJECT_SOURCE CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/src/*.cpp
    ${PROJECT_SOURCE_DIR}/src/*.c
)


set(PYBIND11_FINDPYTHON ON)
find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(_core ${PROJECT_SOURCE})

target_include_directories(_core PRIVATE ${PROJECT_SOURCE_DIR}/wujihandcpp/include)
target_include_directories(_core PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(_core PRIVATE pybind11::module)

target_link_directories(_core PRIVATE ${CMAKE_BINARY_DIR}/wujihandcpp)
target_link_libraries(_core PRIVATE wujihandcpp)

# Apply WUJI_SCOPE_DEBUG to _core target
if(WUJI_SCOPE_DEBUG)
    target_compile_definitions(_core PRIVATE WUJI_SCOPE_DEBUG)
endif()

# The install directory is the output (wheel) directory
install(TARGETS _core DESTINATION wujihandpy)
