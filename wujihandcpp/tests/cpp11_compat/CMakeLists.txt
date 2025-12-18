cmake_minimum_required(VERSION 3.14)
project(cpp11_compat_test LANGUAGES CXX)

# Force C++11 standard for this test
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Include directories from the installed library
# In a real installation scenario, this would be /usr/include
# For testing, we use the source directory
set(WUJIHANDCPP_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include")

add_executable(cpp11_compat_test main.cpp)
target_include_directories(cpp11_compat_test PRIVATE ${WUJIHANDCPP_INCLUDE_DIR})

# We only need to compile, not link (header-only compatibility test)
# So we don't need to link against the actual library
