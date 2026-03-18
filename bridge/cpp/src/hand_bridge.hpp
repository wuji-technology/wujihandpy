#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <zenoh.hxx>

#include <wujihandcpp/device/hand.hpp>

namespace wujihand_bridge {

/// Resource definition matching the Python bridge protocol.
struct ResourceDef {
    std::string path;
    bool can_get;
    bool can_set;
    bool can_sub;
    nlohmann::json json_schema;
};

/// C++ Zenoh bridge for WujiHand, mirroring the Python hand_zenoh_bridge.py.
class HandBridge {
public:
    HandBridge(
        wujihandcpp::device::Hand& hand, std::string serial_number, double pub_rate);

    ~HandBridge();

    // Non-copyable, non-movable
    HandBridge(const HandBridge&) = delete;
    HandBridge& operator=(const HandBridge&) = delete;
    HandBridge(HandBridge&&) = delete;
    HandBridge& operator=(HandBridge&&) = delete;

    /// Open Zenoh session, declare resources, start controller and publisher.
    void start();

    /// Gracefully shutdown: stop publisher, release controller, put offline status.
    void stop();

private:
    // Key helper
    std::string key(const std::string& suffix) const;

    // Build @capability JSON
    std::string build_capability() const;

    // Resource definitions
    static const std::vector<ResourceDef>& resource_defs();

    // Control protocol handler
    void handle_control(zenoh::Query& query);

    // Resource queryable handler
    void handle_resource_query(zenoh::Query& query, const ResourceDef& res);

    // Read a resource, return JSON value
    nlohmann::json read_resource(const std::string& path);

    // Write a resource from JSON value
    void write_resource(const std::string& path, const nlohmann::json& value);

    // Publisher loop (runs in jthread)
    void publish_loop(std::stop_token stop_token);

    // Start / stop realtime controller
    void start_realtime_controller();
    void stop_realtime_controller();

    // Members
    wujihandcpp::device::Hand& hand_;
    std::string sn_;
    std::string sanitized_sn_;
    double pub_rate_;
    double cutoff_freq_ = 5.0; // LowPass filter for smooth interpolation

    // Zenoh resources
    std::optional<zenoh::Session> session_;
    std::optional<zenoh::LivelinessToken> alive_token_;
    std::vector<zenoh::Queryable<void>> queryables_;
    std::vector<zenoh::Subscriber<void>> subscribers_; // for fire-and-forget writes
    std::vector<zenoh::Publisher> publishers_; // indexed same as sub resources
    std::vector<std::string> pub_paths_;       // paths for sub resources

    // Realtime controller
    std::unique_ptr<wujihandcpp::device::IController> controller_;

    // Thread safety
    std::mutex hand_mutex_;    // Protects SDO read/write operations
    std::mutex control_mutex_; // Protects control_owner_
    std::string control_owner_;

    // Liveliness-based control TTL
    std::optional<zenoh::Subscriber<void>> control_owner_watcher_;
    void start_owner_watcher(const std::string& owner_zid);
    void stop_owner_watcher();
    std::string control_owner_key(const std::string& owner_zid) const;

    // Publisher thread
    std::jthread pub_thread_;
};

} // namespace wujihand_bridge
