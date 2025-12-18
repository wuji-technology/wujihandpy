// C++11 Compatibility Test
// This test verifies that the public headers can be compiled and run with C++11.
// The SDK itself requires C++20, but clients using the installed library
// should be able to include the headers with C++11.

#include <cmath>
#include <cstdio>

#include <wujihandcpp/device/controller.hpp>
#include <wujihandcpp/filter/low_pass.hpp>

int main() {
    using namespace wujihandcpp;

    // Test 1: LowPass filter instantiation and methods
    filter::LowPass lp(10.0);
    if (std::abs(lp.cutoff_freq() - 10.0) > 0.001) {
        std::printf("FAIL: LowPass cutoff_freq mismatch\n");
        return 1;
    }

    // Test 2: calculate_alpha static method
    double alpha = filter::LowPass::calculate_alpha(10.0, 1000.0);
    if (alpha <= 0.0 || alpha >= 1.0) {
        std::printf("FAIL: calculate_alpha returned invalid value: %f\n", alpha);
        return 1;
    }

    // Test 3: LowPass::Unit instantiation and usage
    filter::LowPass::Unit unit;
    unit.reset(lp, 0.5);
    unit.input(lp, 1.0);
    lp.setup(1000.0);
    double output = unit.step(lp);
    if (std::isnan(output)) {
        std::printf("FAIL: LowPass::Unit::step returned NaN\n");
        return 1;
    }

    // Test 4: IController interface (virtual class)
    // Just verify the type exists and has expected interface
    (void)sizeof(device::IController);
    (void)sizeof(device::IRealtimeController);
    (void)sizeof(device::IRealtimeController::JointPositions);

    std::printf("OK: All C++11 compatibility tests passed\n");
    return 0;
}
