#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <wujihandcpp/device/hand.hpp>
#include <wujihandcpp/utility/logging.hpp>

#include "hand_bridge.hpp"

static std::atomic<bool> g_running{true};

static void signal_handler(int /*sig*/) {
    g_running.store(false, std::memory_order_relaxed);
}

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --sn <serial>     Hand serial number filter\n"
              << "  --pub-rate <hz>   Position publish rate in Hz (required, e.g. 1000)\n"
              << "  --log-level <lvl> Log level: trace/debug/info/warn/err/off (default: info)\n"
              << "  --help            Show this help\n";
}

static wujihandcpp::logging::Level parse_log_level(const std::string& s) {
    if (s == "trace") return wujihandcpp::logging::Level::TRACE;
    if (s == "debug") return wujihandcpp::logging::Level::DEBUG;
    if (s == "info") return wujihandcpp::logging::Level::INFO;
    if (s == "warn") return wujihandcpp::logging::Level::WARN;
    if (s == "err" || s == "error") return wujihandcpp::logging::Level::ERR;
    if (s == "off") return wujihandcpp::logging::Level::OFF;
    return wujihandcpp::logging::Level::INFO;
}

int main(int argc, char* argv[]) {
    // Parse arguments
    const char* sn_filter = nullptr;
    double pub_rate = 0.0;
    std::string log_level_str = "info";

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--sn") == 0 && i + 1 < argc) {
            sn_filter = argv[++i];
        } else if (std::strcmp(argv[i], "--pub-rate") == 0 && i + 1 < argc) {
            pub_rate = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            log_level_str = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << argv[i] << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (pub_rate <= 0.0) {
        std::cerr << "Error: --pub-rate is required (e.g. --pub-rate 1000)\n";
        print_usage(argv[0]);
        return 1;
    }

    // Configure logging
    wujihandcpp::logging::set_log_to_console(true);
    wujihandcpp::logging::set_log_level(parse_log_level(log_level_str));

    // Install signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Connect to hand
    std::string info_msg = "Connecting to hand...";
    wujihandcpp::logging::log(
        wujihandcpp::logging::Level::INFO, info_msg.c_str(), info_msg.size());

    wujihandcpp::device::Hand hand(sn_filter);

    // Read product serial number
    std::string sn;
    try {
        sn = hand.read_product_sn();
    } catch (const std::exception&) {
        // Firmware too old to support SN read
    }
    if (sn.empty()) {
        sn = "WUJIHAND_" + std::to_string(reinterpret_cast<uintptr_t>(&hand));
        std::string warn_msg = "Could not read product SN, using fallback: " + sn;
        wujihandcpp::logging::log(
            wujihandcpp::logging::Level::WARN, warn_msg.c_str(), warn_msg.size());
    }

    info_msg = "Hand connected, SN: " + sn;
    wujihandcpp::logging::log(
        wujihandcpp::logging::Level::INFO, info_msg.c_str(), info_msg.size());

    // Create and start bridge
    wujihand_bridge::HandBridge bridge(hand, sn, pub_rate);
    bridge.start();

    info_msg = "Bridge running. Press Ctrl+C to stop.";
    wujihandcpp::logging::log(
        wujihandcpp::logging::Level::INFO, info_msg.c_str(), info_msg.size());

    // Wait loop
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Graceful shutdown
    bridge.stop();

    info_msg = "Exiting.";
    wujihandcpp::logging::log(
        wujihandcpp::logging::Level::INFO, info_msg.c_str(), info_msg.size());

    return 0;
}
