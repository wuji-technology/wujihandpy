/// Zenoh Bridge Demo Playback (C++)
///
/// Plays a recorded hand trajectory through the Zenoh bridge.
/// Flow: discover -> acquire -> enable -> playback -> disable -> release.
///
/// Prerequisites:
///   1. Hand connected via USB
///   2. C++ bridge running: ./wujihand_zenoh_bridge
///
/// Usage:
///   ./zenoh_demo [--sn LQSQJR_251128_004] [--speed 1.0] [--loop] [--trajectory path.json]

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

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
        if (reply.is_ok()) {
            return reply.get_ok().get_payload().as_string();
        }
    }
    throw std::runtime_error("No reply for: " + key_expr);
}

static json get_resource(zenoh::Session& session, const std::string& sn,
                         const std::string& path) {
    auto raw = zenoh_get_string(session, "wuji/" + sn + "/" + path);
    return json::parse(raw);
}

static void set_resource(zenoh::Session& session, const std::string& sn,
                         const std::string& path, const json& value) {
    auto result = zenoh_get_string(session, "wuji/" + sn + "/" + path, value.dump());
    if (result != "\"ok\"")
        throw std::runtime_error("SET " + path + " failed: " + result);
}

// ---------------------------------------------------------------------------
// Discovery & control
// ---------------------------------------------------------------------------

static std::string find_hand(zenoh::Session& session, const std::string& sn_hint) {
    if (!sn_hint.empty()) {
        auto raw = zenoh_get_string(session, "wuji/" + sn_hint + "/@capability");
        auto cap = json::parse(raw);
        std::cout << "Found hand: " << cap["serial_number"].get<std::string>() << " ("
                  << cap["resources"].size() << " resources)\n";
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
                // Extract SN between "wuji/" and "/@alive"
                auto discovered_sn = key.substr(5, pos - 5);
                std::cout << "Discovered: " << discovered_sn << "\n";
                return discovered_sn;
            }
        }
    }
    throw std::runtime_error("No hand found on Zenoh network");
}

static std::string acquire_control(zenoh::Session& session, const std::string& sn) {
    auto zid = session.get_zid().to_string();
    auto result = zenoh_get_string(session, "wuji/" + sn + "/@control",
                                   "acquire:" + zid);
    if (result == "granted") {
        std::cout << "Control acquired (ZID: " << zid.substr(0, 8) << "...)\n";
        return zid;
    }
    throw std::runtime_error("Control denied: " + result);
}

static void release_control(zenoh::Session& session, const std::string& sn,
                             const std::string& zid) {
    auto result = zenoh_get_string(session, "wuji/" + sn + "/@control",
                                   "release:" + zid);
    std::cout << "Control released: " << result << "\n";
}

// ---------------------------------------------------------------------------
// Trajectory
// ---------------------------------------------------------------------------

struct Trajectory {
    std::vector<std::vector<double>> frames; // each frame: 20 doubles
    std::string handedness;
};

static Trajectory load_trajectory(const std::string& path, const std::string& hand) {
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Cannot open: " + path);

    json data = json::parse(f);
    std::string key = hand + "_angles";
    if (!data.contains(key))
        throw std::runtime_error("No '" + key + "' in " + path);

    Trajectory traj;
    traj.handedness = hand;
    traj.frames = data[key].get<std::vector<std::vector<double>>>();

    std::printf("Loaded trajectory: %zu frames (%.1fs at 1.0x)\n", traj.frames.size(),
                traj.frames.size() * 0.01);
    return traj;
}

static json frame_to_5x4(const std::vector<double>& frame) {
    json arr = json::array();
    for (int i = 0; i < 5; i++) {
        json row = json::array();
        for (int j = 0; j < 4; j++)
            row.push_back(frame[i * 4 + j]);
        arr.push_back(std::move(row));
    }
    return arr;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Parse arguments
    std::string sn_hint;
    double speed = 1.0;
    bool loop = false;
    std::string trajectory_path;
    std::string hand_override;

    // Default trajectory path: relative to executable or hardcoded
    {
        // Try to find default trajectory
        const char* candidates[] = {
            "example/data/demo_record_angles.json",
            "../example/data/demo_record_angles.json",
            "../../example/data/demo_record_angles.json",
        };
        for (auto c : candidates) {
            std::ifstream test(c);
            if (test.good()) {
                trajectory_path = c;
                break;
            }
        }
    }

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--sn") == 0 && i + 1 < argc)
            sn_hint = argv[++i];
        else if (std::strcmp(argv[i], "--speed") == 0 && i + 1 < argc)
            speed = std::clamp(std::atof(argv[++i]), 0.5, 3.0);
        else if (std::strcmp(argv[i], "--loop") == 0)
            loop = true;
        else if (std::strcmp(argv[i], "--trajectory") == 0 && i + 1 < argc)
            trajectory_path = argv[++i];
        else if (std::strcmp(argv[i], "--hand") == 0 && i + 1 < argc)
            hand_override = argv[++i];
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cerr << "Usage: " << argv[0]
                      << " [--sn SN] [--speed 1.0] [--loop] [--trajectory path.json] [--hand left|right]\n";
            return 0;
        }
    }

    if (trajectory_path.empty()) {
        std::cerr << "Error: no trajectory file found. Use --trajectory <path>\n";
        return 1;
    }

    // Open Zenoh session
    auto config = zenoh::Config::create_default();
    auto session = zenoh::Session::open(std::move(config));
    std::cout << "Zenoh session opened\n";

    std::string sn = find_hand(session, sn_hint);
    std::string zid = acquire_control(session, sn);

    // Auto-detect handedness
    if (hand_override.empty()) {
        auto h = get_resource(session, sn, "handedness");
        hand_override = (h.get<int>() == 1) ? "right" : "left";
        std::cout << "Auto-detected handedness: " << hand_override << "\n";
    }

    auto traj = load_trajectory(trajectory_path, hand_override);

    // Subscribe to feedback
    std::atomic<bool> has_pos{false}, has_effort{false};
    json latest_pos, latest_effort;
    std::mutex feedback_mutex;

    auto sub_pos = session.declare_subscriber(
        zenoh::KeyExpr("wuji/" + sn + "/joint/actual_position"),
        [&](zenoh::Sample& sample) {
            std::lock_guard lock(feedback_mutex);
            latest_pos = json::parse(sample.get_payload().as_string());
            has_pos.store(true, std::memory_order_relaxed);
        },
        []() {} // on_drop
    );

    auto sub_effort = session.declare_subscriber(
        zenoh::KeyExpr("wuji/" + sn + "/joint/actual_effort"),
        [&](zenoh::Sample& sample) {
            std::lock_guard lock(feedback_mutex);
            latest_effort = json::parse(sample.get_payload().as_string());
            has_effort.store(true, std::memory_order_relaxed);
        },
        []() {} // on_drop
    );

    try {
        // Set effort limit
        constexpr double SAFE_EFFORT_LIMIT = 1.5;
        std::printf("Setting effort limit to %.1fA...\n", SAFE_EFFORT_LIMIT);
        json effort_arr = json::array();
        for (int i = 0; i < 5; i++)
            effort_arr.push_back(json::array({SAFE_EFFORT_LIMIT, SAFE_EFFORT_LIMIT,
                                              SAFE_EFFORT_LIMIT, SAFE_EFFORT_LIMIT}));
        set_resource(session, sn, "joint/effort_limit", effort_arr);

        // Enable joints
        std::cout << "Enabling all joints...\n";
        json enabled_arr = json::array();
        for (int i = 0; i < 5; i++)
            enabled_arr.push_back(json::array({true, true, true, true}));
        set_resource(session, sn, "joint/enabled", enabled_arr);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Read effort limits for percentage display
        auto effort_limit = get_resource(session, sn, "joint/effort_limit");

        const double interval_s = 0.01 / speed;
        const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(interval_s));
        int loop_count = 0;

        while (g_running.load(std::memory_order_relaxed)) {
            loop_count++;
            std::string suffix = loop ? " (loop " + std::to_string(loop_count) + ")" : "";
            std::printf("\nPlaying trajectory: %zu frames at %.1fx speed%s\n",
                        traj.frames.size(), speed, suffix.c_str());

            auto next_tick = std::chrono::steady_clock::now();

            for (size_t i = 0; i < traj.frames.size() && g_running.load(std::memory_order_relaxed); i++) {
                auto target = frame_to_5x4(traj.frames[i]);
                set_resource(session, sn, "joint/target_position", target);

                // Print feedback every 50 frames
                if (i % 50 == 0 && has_pos.load(std::memory_order_relaxed)) {
                    std::lock_guard lock(feedback_mutex);
                    // F2 tracking error
                    double err_f2[4];
                    for (int j = 0; j < 4; j++)
                        err_f2[j] = target[1][j].get<double>() - latest_pos[1][j].get<double>();

                    std::string effort_str;
                    if (has_effort.load(std::memory_order_relaxed)) {
                        double pct[4];
                        for (int j = 0; j < 4; j++) {
                            double eff = latest_effort[1][j].get<double>();
                            double lim = effort_limit[1][j].get<double>();
                            pct[j] = (lim != 0) ? (eff / lim * 100.0) : 0.0;
                        }
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), "  effort%%=[%.0f,%.0f,%.0f,%.0f]",
                                      pct[0], pct[1], pct[2], pct[3]);
                        effort_str = buf;
                    }

                    double progress = (i + 1.0) / traj.frames.size() * 100.0;
                    std::printf("\r  [%5.1f%%] frame %zu/%zu  F2 err=[%+.2f,%+.2f,%+.2f,%+.2f]%s",
                                progress, i + 1, traj.frames.size(),
                                err_f2[0], err_f2[1], err_f2[2], err_f2[3],
                                effort_str.c_str());
                    std::fflush(stdout);
                }

                next_tick += interval;
                std::this_thread::sleep_until(next_tick);
            }

            std::printf("\n  Playback complete (%.1fs)\n",
                        traj.frames.size() * interval_s);

            if (!loop)
                break;
        }

    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
    }

    // Cleanup
    std::cout << "Returning to zero position...\n";
    json zero = json::array();
    for (int i = 0; i < 5; i++)
        zero.push_back(json::array({0.0, 0.0, 0.0, 0.0}));
    try {
        set_resource(session, sn, "joint/target_position", zero);
    } catch (...) {}
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "Disabling joints...\n";
    json disabled = json::array();
    for (int i = 0; i < 5; i++)
        disabled.push_back(json::array({false, false, false, false}));
    try {
        set_resource(session, sn, "joint/enabled", disabled);
    } catch (...) {}

    release_control(session, sn, zid);
    std::cout << "Done.\n";
    return 0;
}
