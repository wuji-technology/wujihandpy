#include <cmath>
#include <csignal>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#include <wujihandcpp/data/hand.hpp>
#include <wujihandcpp/device/hand.hpp>
#include <wujihandcpp/device/latch.hpp>
#include <wujihandcpp/utility/fps_counter.hpp>

using namespace wujihandcpp;

int main() {
    static std::atomic<bool> running{true};
    std::signal(SIGINT, [](int) { running.store(false, std::memory_order_relaxed); });

    device::Hand hand;

    // Set control mode
    hand.write<data::joint::ControlMode>(2);

    // Enable all joints
    hand.write<data::joint::ControlWord>(1);

    // Return all joints to initial point
    using ControlPosition = data::joint::ControlPosition;
    hand.write<ControlPosition>(0.0);

    // Disable non-index fingers
    device::Latch latch;
    for (int i = 0; i < 5; i++)
        if (i != 1)
            hand.finger(i).write_async<data::joint::ControlWord>(latch, 5);
    latch.wait();

    // 2Hz SDO Control
    constexpr double update_rate = 2.0;
    constexpr auto update_period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / update_rate));

    auto next_iteration_time = std::chrono::steady_clock::now();
    double x = 0;
    while (running.load(std::memory_order_relaxed)) {
        double y = (1 - std::cos(x)) * 0.8;

        hand.finger(1).joint(0).write_async_unchecked<ControlPosition>(y);
        hand.finger(1).joint(2).write_async_unchecked<ControlPosition>(y);
        hand.finger(1).joint(3).write_async_unchecked<ControlPosition>(y);

        x += M_PI / update_rate;
        next_iteration_time += update_period;
        std::this_thread::sleep_until(next_iteration_time);
    }

    // Disable the entire hand
    hand.write<data::joint::ControlWord>(5);
    std::cout << "Program exited correctly.\n";
}