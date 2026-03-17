/// Zenoh Bridge Realtime Control Example (C++)
///
/// Connects to the hand through Zenoh and runs a sine wave on F2-F5,
/// matching the behavior of example/3.realtime.py and example/6.zenoh_realtime.py.
///
/// Prerequisites:
///   1. Hand connected via USB
///   2. C++ bridge running: ./wujihand_zenoh_bridge
///
/// Usage:
///   ./zenoh_realtime [--sn LQSQJR_251128_004] [--duration 10] [--rate 50]

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <variant>

#include <nlohmann/json.hpp>
#include <zenoh.hxx>

using json = nlohmann::json;

static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running.store(false, std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// Zenoh helpers
// ---------------------------------------------------------------------------

static std::string zenoh_get_string(zenoh::Session& session, const std::string& key_expr,
                                    const std::string& payload = "", double timeout_s = 5.0) {
    zenoh::Session::GetOptions opts;
    opts.timeout_ms = static_cast<uint64_t>(timeout_s * 1000);
    if (!payload.empty())
        opts.payload = zenoh::Bytes(payload);

    auto handler = session.get(zenoh::KeyExpr(key_expr), "",
                               zenoh::channels::FifoChannel(16), std::move(opts));
    for (auto res = handler.recv(); std::holds_alternative<zenoh::Reply>(res);
         res = handler.recv()) {
        const auto& reply = std::get<zenoh::Reply>(res);
        if (reply.is_ok())
            return reply.get_ok().get_payload().as_string();
    }
    throw std::runtime_error("No reply for: " + key_expr);
}

static json get_resource(zenoh::Session& session, const std::string& sn,
                         const std::string& path) {
    return json::parse(zenoh_get_string(session, "wuji/" + sn + "/" + path));
}

static void set_resource(zenoh::Session& session, const std::string& sn,
                         const std::string& path, const json& value) {
    auto result = zenoh_get_string(session, "wuji/" + sn + "/" + path, value.dump());
    if (result != "\"ok\"")
        throw std::runtime_error("SET " + path + " failed: " + result);
}

static std::string find_hand(zenoh::Session& session, const std::string& sn_hint) {
    if (!sn_hint.empty()) {
        auto cap = json::parse(
            zenoh_get_string(session, "wuji/" + sn_hint + "/@capability"));
        std::printf("Found hand: %s (%zu resources)\n",
                    cap["serial_number"].get<std::string>().c_str(),
                    cap["resources"].size());
        return sn_hint;
    }

    std::cout << "Scanning for hands on Zenoh network...\n";
    zenoh::Session::LivelinessGetOptions opts;
    opts.timeout_ms = 3000;
    auto handler = session.liveliness_get(zenoh::KeyExpr("wuji/**"),
                                          zenoh::channels::FifoChannel(16), std::move(opts));
    for (auto res = handler.recv(); std::holds_alternative<zenoh::Reply>(res);
         res = handler.recv()) {
        const auto& reply = std::get<zenoh::Reply>(res);
        if (reply.is_ok()) {
            auto sv = reply.get_ok().get_keyexpr().as_string_view();
            std::string key(sv.data(), sv.size());
            auto pos = key.find("/@alive");
            if (pos != std::string::npos) {
                auto sn = key.substr(5, pos - 5);
                std::cout << "Discovered: " << sn << "\n";
                return sn;
            }
        }
    }
    throw std::runtime_error("No hand found on Zenoh network");
}

static std::string acquire_control(zenoh::Session& session, const std::string& sn) {
    auto zid = session.get_zid().to_string();
    auto result = zenoh_get_string(session, "wuji/" + sn + "/@control", "acquire:" + zid);
    if (result == "granted") {
        std::printf("Control acquired (ZID: %.8s...)\n", zid.c_str());
        return zid;
    }
    throw std::runtime_error("Control denied: " + result);
}

static void release_control(zenoh::Session& session, const std::string& sn,
                             const std::string& zid) {
    auto result = zenoh_get_string(session, "wuji/" + sn + "/@control", "release:" + zid);
    std::cout << "Control released: " << result << "\n";
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string sn_hint;
    double duration = 10.0;
    double rate = 50.0;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--sn") == 0 && i + 1 < argc)
            sn_hint = argv[++i];
        else if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            duration = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc)
            rate = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cerr << "Usage: " << argv[0]
                      << " [--sn SN] [--duration 10] [--rate 50]\n";
            return 0;
        }
    }

    auto config = zenoh::Config::create_default();
    auto session = zenoh::Session::open(std::move(config));
    std::cout << "Zenoh session opened\n";

    std::string sn = find_hand(session, sn_hint);
    std::string zid = acquire_control(session, sn);

    // Subscribe to feedback
    json latest_pos, latest_effort;
    std::atomic<bool> has_pos{false}, has_effort{false};
    std::mutex feedback_mutex;

    auto sub_pos = session.declare_subscriber(
        zenoh::KeyExpr("wuji/" + sn + "/joint/actual_position"),
        [&](zenoh::Sample& sample) {
            std::lock_guard lock(feedback_mutex);
            latest_pos = json::parse(sample.get_payload().as_string());
            has_pos.store(true, std::memory_order_relaxed);
        },
        []() {});

    auto sub_effort = session.declare_subscriber(
        zenoh::KeyExpr("wuji/" + sn + "/joint/actual_effort"),
        [&](zenoh::Sample& sample) {
            std::lock_guard lock(feedback_mutex);
            latest_effort = json::parse(sample.get_payload().as_string());
            has_effort.store(true, std::memory_order_relaxed);
        },
        []() {});

    try {
        // Enable joints
        std::cout << "Enabling all joints...\n";
        set_resource(session, sn, "joint/enabled",
                     json::array({{true, true, true, true},
                                  {true, true, true, true},
                                  {true, true, true, true},
                                  {true, true, true, true},
                                  {true, true, true, true}}));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Read effort limits
        auto effort_limit = get_resource(session, sn, "joint/effort_limit");
        std::printf("Effort limits: [%.2f, %.2f, %.2f, %.2f]\n",
                    effort_limit[0][0].get<double>(), effort_limit[0][1].get<double>(),
                    effort_limit[0][2].get<double>(), effort_limit[0][3].get<double>());

        // Sine wave control loop
        std::printf("Running sine wave for %.0fs at %.0fHz...\n", duration, rate);
        std::cout << "  F1 (thumb) stays still, F2-F5 do sine on J1/J3/J4\n";

        const double period = 1.0 / rate;
        const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(period));

        double x = 0.0;
        auto t_start = std::chrono::steady_clock::now();
        auto next_tick = t_start;

        while (g_running.load(std::memory_order_relaxed)) {
            auto elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t_start).count();
            if (elapsed >= duration)
                break;

            double y = (1.0 - std::cos(x)) * 0.8;

            json target = json::array({
                {0.0, 0.0, 0.0, 0.0},  // F1 (thumb)
                {y,   0.0, y,   y  },  // F2 (index)
                {y,   0.0, y,   y  },  // F3 (middle)
                {y,   0.0, y,   y  },  // F4 (ring)
                {y,   0.0, y,   y  },  // F5 (pinky)
            });

            set_resource(session, sn, "joint/target_position", target);

            // Print feedback
            if (has_pos.load(std::memory_order_relaxed)) {
                std::lock_guard lock(feedback_mutex);
                double actual_f2_j1 = latest_pos[1][0].get<double>();
                double err = y - actual_f2_j1;

                std::string effort_str;
                if (has_effort.load(std::memory_order_relaxed)) {
                    double pct[3];
                    for (int idx = 0; idx < 3; idx++) {
                        int j = (idx == 0) ? 0 : (idx == 1) ? 2 : 3;
                        double eff = latest_effort[1][j].get<double>();
                        double lim = effort_limit[1][j].get<double>();
                        pct[idx] = (lim != 0) ? (eff / lim * 100.0) : 0.0;
                    }
                    char buf[48];
                    std::snprintf(buf, sizeof(buf), "  effort%%=[%.0f,%.0f,%.0f]",
                                  pct[0], pct[1], pct[2]);
                    effort_str = buf;
                }

                std::printf("\r  y=%.2f  actual=%.2f  err=%.3f%s",
                            y, actual_f2_j1, err, effort_str.c_str());
                std::fflush(stdout);
            }

            x += M_PI / rate;
            next_tick += interval;
            std::this_thread::sleep_until(next_tick);
        }

        std::printf("\n\nSine wave complete.\n");

    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nError: %s\n", e.what());
    }

    // Cleanup
    std::cout << "Returning to zero position...\n";
    try {
        set_resource(session, sn, "joint/target_position",
                     json::array({{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
                                  {0, 0, 0, 0}, {0, 0, 0, 0}}));
    } catch (...) {}
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "Disabling joints...\n";
    try {
        set_resource(session, sn, "joint/enabled",
                     json::array({{false, false, false, false},
                                  {false, false, false, false},
                                  {false, false, false, false},
                                  {false, false, false, false},
                                  {false, false, false, false}}));
    } catch (...) {}

    release_control(session, sn, zid);
    std::cout << "Done.\n";
    return 0;
}
