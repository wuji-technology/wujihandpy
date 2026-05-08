#include <cmath>
#include <csignal>
#include <cstdint>

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

    // Set control mode & enable all joints
    hand.write<data::joint::ControlMode>(2);
    hand.write<data::joint::ControlWord>(1);

    // Return all joints to initial point
    hand.write<data::joint::ControlPosition>(0.0);
    hand.finger(1).joint(1).write<data::joint::ControlPosition>(0.1);
    hand.finger(4).joint(1).write<data::joint::ControlPosition>(-0.1);

    // Wait for joints to move into place
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    hand.write<data::joint::ControlWord>(5);

    // Enable CSP & PDO Control
    hand.write<data::joint::ControlMode>(4);
    hand.write<data::hand::GlobalTpdoId>(1);
    hand.write<data::hand::JointPdoInterval>(1000);
    hand.write<data::hand::PdoEnabled>(1);
    hand.write<data::joint::ControlWord>(1);

    // Disable the whole thumb & each J2
    hand.finger(0).write<data::joint::ControlWord>(5);
    for (int i = 1; i < 5; i++)
        hand.finger(i).joint(1).write<data::joint::ControlWord>(5);

    // 1kHz SDO Control
    constexpr double update_rate = 1000.0;
    constexpr auto update_period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / update_rate));

    auto begin = std::chrono::steady_clock::now();
    auto next_iteration_time = begin;

    utility::FpsCounter fps_counter;
    double x = 0;

    double control_positions[5][4];
    control_positions[0][0] = 0.0;
    control_positions[0][1] = 0.0;
    control_positions[0][2] = 0.0;
    control_positions[0][3] = 0.0;

    while (running.load(std::memory_order_relaxed)) {
        if (fps_counter.count())
            std::cout << "PDO Control Actual Fps: " << fps_counter.fps() << '\n';

        double y = (1 - std::cos(x)) * 0.8;
        for (int i = 1; i < 5; i++) {
            control_positions[i][0] = y;
            control_positions[i][1] = 0.0;
            control_positions[i][2] = y;
            control_positions[i][3] = y;
        }
        auto duration =
            std::chrono::duration_cast<std::chrono::microseconds>(next_iteration_time - begin);
        hand.pdo_write_async_unchecked(control_positions, static_cast<uint32_t>(duration.count()));

        x += M_PI / update_rate;
        next_iteration_time += update_period;
        std::this_thread::sleep_until(next_iteration_time);
    }

    // Disable the entire hand
    hand.write<data::joint::ControlWord>(5);
    std::cout << "Program exited correctly.\n";
}