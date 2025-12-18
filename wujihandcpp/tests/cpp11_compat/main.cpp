// C++11 Compatibility Test
// This test verifies that the public headers can be compiled with C++11 standard.
// The SDK itself requires C++20, but clients using the installed library
// should be able to include the headers with C++11.

// Include all public headers
#include <wujihandcpp/device/hand.hpp>
#include <wujihandcpp/device/finger.hpp>
#include <wujihandcpp/device/joint.hpp>
#include <wujihandcpp/device/controller.hpp>
#include <wujihandcpp/filter/low_pass.hpp>
#include <wujihandcpp/utility/logging.hpp>

// This file should compile successfully with -std=c++11
// If it fails, it means the public headers contain C++14/17/20 features
// that need to be removed or guarded.

int main() {
    // Just a compilation test, no runtime behavior needed
    return 0;
}
