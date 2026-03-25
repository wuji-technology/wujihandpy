#include "hand_bridge.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>

#include <wujihandcpp/data/hand.hpp>
#include <wujihandcpp/data/joint.hpp>
#include <wujihandcpp/device/controller.hpp>
#include <wujihandcpp/device/latch.hpp>
#include <wujihandcpp/filter/low_pass.hpp>
#include <wujihandcpp/utility/logging.hpp>

#include "json_helpers.hpp"

namespace wujihand_bridge {

using namespace wujihandcpp;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Timestamp utility
// ---------------------------------------------------------------------------

/// Return current UTC time as microseconds since Unix epoch.
static int64_t get_timestamp_us() {
    auto now = std::chrono::system_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch());
    return us.count();
}

/// Wrap a JSON data value with a host-side timestamp.
/// Output: {"timestamp_us": <int64>, "data": <value>}
static json wrap_with_timestamp(const json& value, int64_t timestamp_us = 0) {
    if (timestamp_us == 0) {
        timestamp_us = get_timestamp_us();
    }
    return json{{"timestamp_us", timestamp_us}, {"data", value}};
}

// ---------------------------------------------------------------------------
// Logging helper
// ---------------------------------------------------------------------------
static void log_info(const std::string& msg) {
    logging::log(logging::Level::INFO, msg.c_str(), msg.size());
}

static void log_warn(const std::string& msg) {
    logging::log(logging::Level::WARN, msg.c_str(), msg.size());
}

static void log_error(const std::string& msg) {
    logging::log(logging::Level::ERR, msg.c_str(), msg.size());
}

static std::optional<std::string> requester_id_from_attachment(
    const std::optional<std::reference_wrapper<const zenoh::Bytes>>& attachment) {
    if (!attachment.has_value()) {
        return std::nullopt;
    }
    auto requester = attachment->get().as_string();
    if (requester.empty()) {
        return std::nullopt;
    }
    return requester;
}

// ---------------------------------------------------------------------------
// Resource definitions (must match Python bridge exactly)
// ---------------------------------------------------------------------------
const std::vector<ResourceDef>& HandBridge::resource_defs() {
    static const std::vector<ResourceDef> defs = {
        // GET-only scalar resources
        {"input_voltage", true, false, false,
         {{"title", "InputVoltage"}, {"type", "number"}}},
        {"temperature", true, false, false,
         {{"title", "Temperature"}, {"type", "number"}}},
        {"handedness", true, false, false,
         {{"title", "Handedness"}, {"type", "integer"}}},
        {"firmware_version", true, false, false,
         {{"title", "FirmwareVersion"}, {"type", "integer"}}},

        // GET-only array resources
        {"joint/actual_position", true, false, true,
         {{"title", "JointActualPosition"},
          {"type", "array"},
          {"description", "5x4 joint positions (5 fingers x 4 joints)"},
          {"items", {{"type", "array"}, {"items", {{"type", "number"}}}}}}},
        {"joint/temperature", true, false, false,
         {{"title", "JointTemperature"},
          {"type", "array"},
          {"items", {{"type", "array"}, {"items", {{"type", "number"}}}}}}},
        {"joint/error_code", true, false, false,
         {{"title", "JointErrorCode"},
          {"type", "array"},
          {"items", {{"type", "array"}, {"items", {{"type", "integer"}}}}}}},
        {"joint/effort_limit", true, true, false,
         {{"title", "JointEffortLimit"},
          {"type", "array"},
          {"items", {{"type", "array"}, {"items", {{"type", "number"}}}}}}},
        {"joint/upper_limit", true, false, false,
         {{"title", "JointUpperLimit"},
          {"type", "array"},
          {"items", {{"type", "array"}, {"items", {{"type", "number"}}}}}}},
        {"joint/lower_limit", true, false, false,
         {{"title", "JointLowerLimit"},
          {"type", "array"},
          {"items", {{"type", "array"}, {"items", {{"type", "number"}}}}}}},
        {"joint/bus_voltage", true, false, false,
         {{"title", "JointBusVoltage"},
          {"type", "array"},
          {"description", "5x4 joint bus voltages"},
          {"items", {{"type", "array"}, {"items", {{"type", "number"}}}}}}},
        {"joint/actual_effort", true, false, true,
         {{"title", "JointActualEffort"},
          {"type", "array"},
          {"description", "5x4 joint actual effort (requires firmware >= 1.2.0)"},
          {"items", {{"type", "array"}, {"items", {{"type", "number"}}}}}}},

        // SET resources (require control)
        {"joint/reset_error", false, true, false,
         {{"title", "JointResetError"},
          {"type", "array"},
          {"description", "5x4 joint error reset (write non-zero to reset)"},
          {"items", {{"type", "array"}, {"items", {{"type", "integer"}}}}}}},
        {"joint/control_mode", false, true, false,
         {{"title", "JointControlMode"},
          {"type", "array"},
          {"description", "5x4 joint control modes"},
          {"items", {{"type", "array"}, {"items", {{"type", "integer"}}}}}}},
        {"joint/enabled", false, true, false,
         {{"title", "JointEnabled"},
          {"type", "array"},
          {"description", "5x4 joint enabled states"},
          {"items", {{"type", "array"}, {"items", {{"type", "boolean"}}}}}}},
        {"joint/target_position", false, true, false,
         {{"title", "JointTargetPosition"},
          {"type", "array"},
          {"description", "5x4 joint target positions"},
          {"items", {{"type", "array"}, {"items", {{"type", "number"}}}}}}},
    };
    return defs;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
HandBridge::HandBridge(
    device::Hand& hand, std::string serial_number, double pub_rate)
    : hand_(hand)
    , sn_(std::move(serial_number))
    , pub_rate_(pub_rate) {
    if (pub_rate_ <= 0.0) {
        throw std::invalid_argument("pub_rate must be positive");
    }

    // Sanitize SN: replace '.' with '_' for Zenoh key expressions
    sanitized_sn_ = sn_;
    std::replace(sanitized_sn_.begin(), sanitized_sn_.end(), '.', '_');

    // Allow multi-thread access (we protect with our own mutexes)
    hand_.disable_thread_safe_check();
}

HandBridge::~HandBridge() {
    stop();
}

// ---------------------------------------------------------------------------
// Key helper
// ---------------------------------------------------------------------------
std::string HandBridge::key(const std::string& suffix) const {
    return "wuji/" + sanitized_sn_ + "/" + suffix;
}

// ---------------------------------------------------------------------------
// build_capability
// ---------------------------------------------------------------------------
std::string HandBridge::build_capability() const {
    json resources = json::array();
    for (const auto& r : resource_defs()) {
        json schema = r.json_schema;

        // Wrap SUB resource schemas with timestamp envelope (matches Python bridge)
        if (r.can_sub) {
            schema = {
                {"title", r.json_schema.value("title", "") + "Timestamped"},
                {"type", "object"},
                {"description", "Host-timestamped envelope: {timestamp_us, data}"},
                {"properties", {
                    {"timestamp_us", {{"type", "integer"}, {"description", "UTC microseconds since epoch"}}},
                    {"data", r.json_schema},
                }},
                {"required", json::array({"timestamp_us", "data"})},
            };
        }

        resources.push_back({
            {"path", r.path},
            {"schema_id", 0},
            {"can_get", r.can_get},
            {"can_set", r.can_set},
            {"can_sub", r.can_sub},
            {"can_pub", false},
            {"can_exec", false},
            {"internal", false},
            {"serde_format", "json"},
            {"json_schema", schema},
        });
    }

    json capability = {
        {"device_id", 0},
        {"device_proto", "custom"},
        {"firmware_version", ""},
        {"serial_number", sn_},
        {"nodes", json::array()},
        {"resources", resources},
    };
    return capability.dump();
}

// ---------------------------------------------------------------------------
// start
// ---------------------------------------------------------------------------
void HandBridge::start() {
    log_info("Opening Zenoh session...");
    auto config = zenoh::Config::create_default();
    session_.emplace(zenoh::Session::open(std::move(config)));
    log_info("Zenoh session opened");

    // 1. Liveliness token
    alive_token_.emplace(session_->liveliness_declare_token(zenoh::KeyExpr(key("@alive"))));
    log_info("Liveliness token declared: " + key("@alive"));

    // 2. Start realtime controller (before status/queryables so reads work)
    start_realtime_controller();

    // 3. Status: online
    session_->put(zenoh::KeyExpr(key("@status")), zenoh::Bytes("online"));
    log_info("Status: online");

    // 4. Capability queryable
    auto cap_str = build_capability();
    queryables_.push_back(session_->declare_queryable(
        zenoh::KeyExpr(key("@capability")),
        [this, cap_str](zenoh::Query& query) {
            query.reply(zenoh::KeyExpr(key("@capability")),
                        zenoh::Bytes(cap_str));
        },
        []() {}));
    log_info("@capability queryable declared");

    // 5. Control queryable
    queryables_.push_back(session_->declare_queryable(
        zenoh::KeyExpr(key("@control")),
        [this](zenoh::Query& query) {
            handle_control(query);
        },
        []() {}));
    log_info("@control queryable declared");

    // 6. Resource queryables + publishers for SUB resources
    for (const auto& r : resource_defs()) {
        if (r.can_get || r.can_set) {
            auto r_copy = r;  // capture by value to avoid dangling reference
            queryables_.push_back(session_->declare_queryable(
                zenoh::KeyExpr(key(r.path)),
                [this, r_copy](zenoh::Query& query) {
                    handle_resource_query(query, r_copy);
                },
                []() {}));
            log_info("Resource queryable: " + r.path);
        }
        if (r.can_sub) {
            publishers_.push_back(
                session_->declare_publisher(zenoh::KeyExpr(key(r.path))));
            pub_paths_.push_back(r.path);
        }
    }

    // 7. Subscribe to target_position for fire-and-forget writes (low latency)
    subscribers_.push_back(session_->declare_subscriber(
        zenoh::KeyExpr(key("joint/target_position")),
        [this](zenoh::Sample& sample) {
            try {
                auto requester = requester_id_from_attachment(sample.get_attachment());
                {
                    std::lock_guard lock(control_mutex_);
                    if (control_owner_.empty()) {
                        log_warn("Ignoring target_position PUT without control owner");
                        return;
                    }
                    if (!requester.has_value()) {
                        log_warn("Ignoring target_position PUT without requester attachment");
                        return;
                    }
                    if (*requester != control_owner_) {
                        log_warn(
                            "Ignoring target_position PUT from non-owner requester " + *requester + " (owner=" +
                            control_owner_ + ")");
                        return;
                    }
                }
                auto value = json::parse(sample.get_payload().as_string());
                write_resource("joint/target_position", value);
            } catch (const std::exception& e) {
                log_error(std::string("target_position subscriber error: ") + e.what());
            }
        },
        []() {}));
    log_info("target_position subscriber declared (fire-and-forget path)");

    // 8. Start publisher jthread
    if (!publishers_.empty()) {
        pub_thread_ = std::jthread([this](std::stop_token st) {
            publish_loop(std::move(st));
        });
        log_info("Publisher loop started at " + std::to_string(pub_rate_) + " Hz");
    }

    log_info("Hand Zenoh Bridge fully started");
}

// ---------------------------------------------------------------------------
// stop
// ---------------------------------------------------------------------------
void HandBridge::stop() {
    // 1. Stop publisher thread
    if (pub_thread_.joinable()) {
        pub_thread_.request_stop();
        pub_thread_.join();
    }

    // 2. Release controller
    stop_realtime_controller();

    // 3. Put status offline
    if (session_.has_value()) {
        session_->put(
            zenoh::KeyExpr(key("@status")),
            zenoh::Bytes("offline"));
        log_info("Status: offline");
    }

    // 4. Clear Zenoh resources (RAII)
    stop_owner_watcher();
    subscribers_.clear();
    queryables_.clear();
    publishers_.clear();
    pub_paths_.clear();
    alive_token_.reset();
    session_.reset();

    log_info("Bridge stopped");
}

// ---------------------------------------------------------------------------
// start_realtime_controller
// ---------------------------------------------------------------------------
void HandBridge::start_realtime_controller() {
    log_info("Starting realtime controller...");

    filter::LowPass lp(cutoff_freq_);
    controller_ = hand_.realtime_controller<true>(lp);

    // Enable all joints
    {
        std::lock_guard lock(hand_mutex_);
        hand_.write<data::joint::Enabled>(true);
    }

    log_info("Realtime controller started, joints enabled");
}

// ---------------------------------------------------------------------------
// stop_realtime_controller
// ---------------------------------------------------------------------------
void HandBridge::stop_realtime_controller() {
    if (controller_) {
        log_info("Stopping realtime controller...");
        controller_.reset();
    }

    // Disable all joints
    try {
        std::lock_guard lock(hand_mutex_);
        hand_.write<data::joint::Enabled>(false);
        log_info("Joints disabled");
    } catch (const std::exception& e) {
        log_error(std::string("Failed to disable joints: ") + e.what());
    }
}

// ---------------------------------------------------------------------------
// Liveliness-based control TTL
// ---------------------------------------------------------------------------
std::string HandBridge::control_owner_key(const std::string& owner_zid) const {
    return key("@control_owner/" + owner_zid);
}

void HandBridge::start_owner_watcher(const std::string& owner_zid) {
    stop_owner_watcher();

    auto owner_key = control_owner_key(owner_zid);
    control_owner_watcher_.emplace(session_->liveliness_declare_subscriber(
        zenoh::KeyExpr(owner_key),
        [this, owner_zid](zenoh::Sample& sample) {
            // SampleKind::Z_SAMPLE_KIND_DELETE means liveliness token dropped (owner crashed)
            if (sample.get_kind() == Z_SAMPLE_KIND_DELETE) {
                {
                    std::lock_guard lock(control_mutex_);
                    if (control_owner_ == owner_zid) {
                        log_info("Control owner " + owner_zid + " crashed, auto-releasing");
                        control_owner_.clear();
                    }
                }
                // Keep the watcher alive until explicit release, replacement, or shutdown.
                // Resetting the subscriber from inside its own callback is unsafe.
            }
        },
        []() {}));
    log_info("Owner watcher started for " + owner_zid);
}

void HandBridge::stop_owner_watcher() {
    control_owner_watcher_.reset();
}

// ---------------------------------------------------------------------------
// handle_control
// ---------------------------------------------------------------------------
void HandBridge::handle_control(zenoh::Query& query) {
    auto key_str = key("@control");

    std::string payload_str;
    auto payload_opt = query.get_payload();
    if (payload_opt.has_value()) {
        payload_str = payload_opt->get().as_string();
    }
    auto attachment_requester = requester_id_from_attachment(query.get_attachment());

    if (payload_str.starts_with("acquire:")) {
        auto requester = payload_str.substr(8);
        if (!attachment_requester.has_value() || *attachment_requester != requester) {
            query.reply_err(zenoh::Bytes("identity_mismatch"));
            log_warn(
                "Control acquire rejected: payload requester " + requester + " != attachment requester " +
                attachment_requester.value_or("<missing>"));
            return;
        }

        std::string current_owner;
        {
            std::lock_guard lock(control_mutex_);
            current_owner = control_owner_;
            if (!current_owner.empty() && current_owner != requester) {
                // handled after releasing the mutex
            } else {
                control_owner_ = requester;
                current_owner.clear();
            }
        }
        if (!current_owner.empty()) {
            query.reply(zenoh::KeyExpr(key_str),
                        zenoh::Bytes("denied:" + current_owner));
            log_info("Control denied to " + requester + ", owner: " + current_owner);
            return;
        }

        try {
            start_owner_watcher(requester);
            {
                std::lock_guard lock(control_mutex_);
                if (control_owner_ != requester) {
                    throw std::runtime_error("control owner lost before acquire completed");
                }
            }
            query.reply(zenoh::KeyExpr(key_str),
                        zenoh::Bytes("granted"));
            log_info("Control granted to " + requester);
        } catch (const std::exception& e) {
            {
                std::lock_guard lock(control_mutex_);
                if (control_owner_ == requester) {
                    control_owner_.clear();
                }
            }
            stop_owner_watcher();
            query.reply_err(zenoh::Bytes(std::string(e.what())));
            log_error("Control acquire failed for " + requester + ": " + e.what());
        }
    } else if (payload_str.starts_with("release:")) {
        auto requester = payload_str.substr(8);
        if (!attachment_requester.has_value() || *attachment_requester != requester) {
            query.reply_err(zenoh::Bytes("identity_mismatch"));
            log_warn(
                "Control release rejected: payload requester " + requester + " != attachment requester " +
                attachment_requester.value_or("<missing>"));
            return;
        }

        bool released = false;
        {
            std::lock_guard lock(control_mutex_);
            if (control_owner_ == requester) {
                control_owner_.clear();
                released = true;
            }
        }
        if (released) {
            stop_owner_watcher();
            query.reply(zenoh::KeyExpr(key_str),
                        zenoh::Bytes("released"));
            log_info("Control released by " + requester);
        } else {
            query.reply(zenoh::KeyExpr(key_str),
                        zenoh::Bytes("not_owner"));
        }
    } else {
        std::lock_guard lock(control_mutex_);
        auto owner = control_owner_.empty() ? std::string("none") : control_owner_;
        query.reply(zenoh::KeyExpr(key_str),
                    zenoh::Bytes(owner));
    }
}

// ---------------------------------------------------------------------------
// handle_resource_query
// ---------------------------------------------------------------------------
void HandBridge::handle_resource_query(zenoh::Query& query, const ResourceDef& res) {
    auto key_str = key(res.path);
    auto payload_opt = query.get_payload();

    bool has_payload = false;
    std::string payload_str;
    if (payload_opt.has_value()) {
        payload_str = payload_opt->get().as_string();
        has_payload = !payload_str.empty();
    }

    if (!has_payload) {
        // GET
        if (!res.can_get) {
            query.reply_err(zenoh::Bytes("GET not supported"));
            return;
        }
        try {
            auto value = read_resource(res.path);
            query.reply(zenoh::KeyExpr(key_str),
                        zenoh::Bytes(value.dump()));
        } catch (const std::exception& e) {
            log_error("GET " + res.path + " failed: " + e.what());
            query.reply_err(zenoh::Bytes(std::string(e.what())));
        }
    } else {
        // SET
        if (!res.can_set) {
            query.reply_err(zenoh::Bytes("SET not supported"));
            return;
        }
        auto requester = requester_id_from_attachment(query.get_attachment());
        {
            std::lock_guard lock(control_mutex_);
            if (control_owner_.empty()) {
                query.reply_err(zenoh::Bytes("no control owner"));
                return;
            }
            if (!requester.has_value()) {
                query.reply_err(zenoh::Bytes("missing requester id"));
                log_warn("SET " + res.path + " rejected: missing requester attachment");
                return;
            }
            if (*requester != control_owner_) {
                query.reply_err(zenoh::Bytes("not control owner"));
                log_warn(
                    "SET " + res.path + " rejected: requester " + *requester + " != owner " + control_owner_);
                return;
            }
        }
        try {
            auto value = json::parse(payload_str);
            write_resource(res.path, value);
            query.reply(zenoh::KeyExpr(key_str),
                        zenoh::Bytes("\"ok\""));
        } catch (const std::exception& e) {
            log_error("SET " + res.path + " failed: " + e.what());
            query.reply_err(zenoh::Bytes(std::string(e.what())));
        }
    }
}

// ---------------------------------------------------------------------------
// read_resource
// ---------------------------------------------------------------------------
json HandBridge::read_resource(const std::string& path) {
    // Atomic reads from controller (no lock needed)
    if (path == "joint/actual_position" && controller_) {
        return atomic_array_to_json(controller_->get_joint_actual_position());
    }
    if (path == "joint/actual_effort" && controller_) {
        return atomic_array_to_json(controller_->get_joint_actual_effort());
    }

    // SDO reads require hand_mutex_
    std::lock_guard lock(hand_mutex_);

    if (path == "input_voltage") {
        return static_cast<double>(hand_.read<data::hand::InputVoltage>());
    }
    if (path == "temperature") {
        return static_cast<double>(hand_.read<data::hand::Temperature>());
    }
    if (path == "handedness") {
        return static_cast<int>(hand_.read<data::hand::Handedness>());
    }
    if (path == "firmware_version") {
        return static_cast<uint32_t>(hand_.read<data::hand::FirmwareVersion>());
    }

    // Per-joint array reads with Latch for batch efficiency
    if (path == "joint/actual_position") {
        // Fallback when no controller
        hand_.read<data::joint::ActualPosition>();
        double result[5][4];
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                result[i][j] = hand_.finger(i).joint(j).get<data::joint::ActualPosition>();
        return double_array_to_json(result);
    }

    if (path == "joint/temperature") {
        device::Latch latch;
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                hand_.finger(i).joint(j).read_async<data::joint::Temperature>(latch);
        latch.wait();
        float result[5][4];
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                result[i][j] = hand_.finger(i).joint(j).get<data::joint::Temperature>();
        return scalar_array_to_json(result);
    }

    if (path == "joint/error_code") {
        device::Latch latch;
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                hand_.finger(i).joint(j).read_async<data::joint::ErrorCode>(latch);
        latch.wait();
        uint32_t result[5][4];
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                result[i][j] = hand_.finger(i).joint(j).get<data::joint::ErrorCode>();
        return scalar_array_to_json(result);
    }

    if (path == "joint/effort_limit") {
        device::Latch latch;
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                hand_.finger(i).joint(j).read_async<data::joint::EffortLimit>(latch);
        latch.wait();
        double result[5][4];
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                result[i][j] = hand_.finger(i).joint(j).get<data::joint::EffortLimit>();
        return double_array_to_json(result);
    }

    if (path == "joint/upper_limit") {
        device::Latch latch;
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                hand_.finger(i).joint(j).read_async<data::joint::UpperLimit>(latch);
        latch.wait();
        double result[5][4];
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                result[i][j] = hand_.finger(i).joint(j).get<data::joint::UpperLimit>();
        return double_array_to_json(result);
    }

    if (path == "joint/lower_limit") {
        device::Latch latch;
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                hand_.finger(i).joint(j).read_async<data::joint::LowerLimit>(latch);
        latch.wait();
        double result[5][4];
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                result[i][j] = hand_.finger(i).joint(j).get<data::joint::LowerLimit>();
        return double_array_to_json(result);
    }

    if (path == "joint/bus_voltage") {
        device::Latch latch;
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                hand_.finger(i).joint(j).read_async<data::joint::BusVoltage>(latch);
        latch.wait();
        float result[5][4];
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                result[i][j] = hand_.finger(i).joint(j).get<data::joint::BusVoltage>();
        return scalar_array_to_json(result);
    }

    throw std::runtime_error("Unknown GET resource: " + path);
}

// ---------------------------------------------------------------------------
// write_resource
// ---------------------------------------------------------------------------
void HandBridge::write_resource(const std::string& path, const json& value) {
    // target_position: direct to controller (atomic, no lock needed)
    if (path == "joint/target_position") {
        double pos[5][4];
        json_to_array(value, pos);
        for (int i = 0; i < 5; i++) {
            for (int j = 0; j < 4; j++) {
                if (!std::isfinite(pos[i][j])) {
                    throw std::invalid_argument("target_position contains non-finite values");
                }
            }
        }
        if (controller_) {
            controller_->set_joint_target_position(pos);
        }
        return;
    }

    // All other writes need SDO access
    std::lock_guard lock(hand_mutex_);

    if (path == "joint/control_mode") {
        device::Latch latch;
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                hand_.finger(i).joint(j).write_async<data::joint::ControlMode>(
                    latch, value[i][j].get<uint16_t>());
        latch.wait();
        return;
    }

    if (path == "joint/enabled") {
        device::Latch latch;
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                hand_.finger(i).joint(j).write_async<data::joint::Enabled>(
                    latch, value[i][j].get<bool>());
        latch.wait();
        return;
    }

    if (path == "joint/effort_limit") {
        device::Latch latch;
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                hand_.finger(i).joint(j).write_async<data::joint::EffortLimit>(
                    latch, value[i][j].get<double>());
        latch.wait();
        return;
    }

    if (path == "joint/reset_error") {
        device::Latch latch;
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                hand_.finger(i).joint(j).write_async<data::joint::ResetError>(
                    latch, value[i][j].get<uint16_t>());
        latch.wait();
        return;
    }

    throw std::runtime_error("Unknown SET resource: " + path);
}

// ---------------------------------------------------------------------------
// publish_loop
// ---------------------------------------------------------------------------
void HandBridge::publish_loop(std::stop_token stop_token) {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(1.0 / pub_rate_));

    auto next_tick = clock::now();

    while (!stop_token.stop_requested()) {
        next_tick += period;

        // Capture a single timestamp for all resources in this cycle
        auto timestamp_us = get_timestamp_us();

        for (size_t idx = 0; idx < pub_paths_.size(); idx++) {
            try {
                auto value = read_resource(pub_paths_[idx]);
                auto envelope = wrap_with_timestamp(value, timestamp_us);
                publishers_[idx].put(zenoh::Bytes(envelope.dump()));
            } catch (const std::exception& e) {
                log_error("Publish error for " + pub_paths_[idx] + ": " + e.what());
            }
        }

        std::this_thread::sleep_until(next_tick);
    }
}

} // namespace wujihand_bridge
