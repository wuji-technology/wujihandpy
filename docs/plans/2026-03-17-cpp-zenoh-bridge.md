# C++ Zenoh Bridge Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Migrate the verified Python Zenoh bridge to native C++ using WujiHandCpp SDK + zenoh-cpp, eliminating the Python layer for lower latency and simpler deployment.

**Architecture:** A standalone C++ executable at `bridge/cpp/` that links against `wujihandcpp` (via `add_subdirectory`) and `zenoh-cpp` (via `FetchContent`). The bridge creates a `device::Hand`, starts a `realtime_controller<true>` for PDO control with upstream feedback, registers Zenoh queryables for GET/SET resources, and runs a 100Hz publisher thread for `actual_position`/`actual_effort`. Control protocol (@alive, @capability, @control) follows wuji-sdk convention exactly as the Python bridge.

**Tech Stack:** C++20, CMake 3.24+, wujihandcpp (same repo), zenoh-cpp (FetchContent), nlohmann/json (FetchContent for JSON serialization)

**Reference:** Python bridge at `bridge/hand_zenoh_bridge.py` (545 lines, verified working)

---

## Key Design Decisions

1. **Location:** `bridge/cpp/` - independent CMakeLists, references SDK via `add_subdirectory(../../wujihandcpp)`
2. **Zenoh binding:** zenoh-cpp (C++ wrapper with RAII)
3. **RT control:** Zenoh callback directly calls `controller->set_joint_target_position()` - no extra loop thread
4. **Filter:** LowPass cutoff configurable via Zenoh SET (firmware handles filtering for new firmware; `hand.write<data::joint::PositionFilterCutoffFreq>()`)
5. **Publisher:** Independent `std::jthread` at 100Hz reading from `controller->get_joint_actual_position()`

## File Overview

```
bridge/cpp/
├── CMakeLists.txt              # Build config
├── src/
│   ├── main.cpp                # Entry point, arg parsing, signal handling
│   ├── hand_bridge.hpp         # HandBridge class declaration
│   ├── hand_bridge.cpp         # HandBridge implementation
│   ├── resource_handlers.hpp   # GET/SET handler functions
│   ├── resource_handlers.cpp   # Resource handler implementations
│   └── json_helpers.hpp        # 5x4 array <-> JSON conversion helpers
```

---

### Task 1: CMake Build Setup

**Files:**
- Create: `bridge/cpp/CMakeLists.txt`

**Step 1: Write CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.24)
project(wujihand_zenoh_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# wujihandcpp SDK (same repo)
set(BUILD_STATIC_WUJIHANDCPP ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../wujihandcpp
                 ${CMAKE_CURRENT_BINARY_DIR}/wujihandcpp)

# zenoh-cpp via FetchContent
include(FetchContent)
FetchContent_Declare(
    zenohcxx
    GIT_REPOSITORY https://github.com/eclipse-zenoh/zenoh-cpp.git
    GIT_TAG main
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(zenohcxx)

# nlohmann/json for JSON serialization
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(json)

file(GLOB_RECURSE BRIDGE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

add_executable(${PROJECT_NAME} ${BRIDGE_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(${PROJECT_NAME} PRIVATE
    wujihandcpp
    zenohcxx::zenohc::lib
    nlohmann_json::nlohmann_json
)
```

**Step 2: Verify CMake configuration compiles (with stub main.cpp)**

Create minimal `bridge/cpp/src/main.cpp`:
```cpp
#include <iostream>
int main() {
    std::cout << "wujihand_zenoh_bridge stub\n";
    return 0;
}
```

Run:
```bash
cd bridge/cpp && mkdir -p build && cd build && cmake .. && cmake --build . -j$(nproc)
```

Expected: Successful build (may take a while for FetchContent downloads on first run).

**Step 3: Commit**

```bash
git add bridge/cpp/CMakeLists.txt bridge/cpp/src/main.cpp
git commit -m "feat(bridge/cpp): add CMake build setup with zenoh-cpp and nlohmann/json"
```

---

### Task 2: JSON Helpers for 5x4 Arrays

**Files:**
- Create: `bridge/cpp/src/json_helpers.hpp`

**Step 1: Write json_helpers.hpp**

Provides conversion between `double[5][4]` / `std::atomic<double>[5][4]` and `nlohmann::json` (2D array). Also helpers for `bool[5][4]`, `uint32_t[5][4]`, `float[5][4]`, `uint16_t[5][4]`.

```cpp
#pragma once

#include <atomic>
#include <cstdint>

#include <nlohmann/json.hpp>

namespace bridge {

// Read atomic 5x4 array to JSON
inline nlohmann::json atomic_array_to_json(const std::atomic<double> (&arr)[5][4]) {
    auto result = nlohmann::json::array();
    for (int i = 0; i < 5; i++) {
        auto row = nlohmann::json::array();
        for (int j = 0; j < 4; j++)
            row.push_back(arr[i][j].load(std::memory_order_relaxed));
        result.push_back(std::move(row));
    }
    return result;
}

// Parse JSON 2D array to double[5][4]
inline void json_to_array(const nlohmann::json& j, double (&out)[5][4]) {
    for (int i = 0; i < 5; i++)
        for (int jj = 0; jj < 4; jj++)
            out[i][jj] = j[i][jj].get<double>();
}

// Parse JSON 2D array to bool (for enabled)
inline void json_to_bool_array(const nlohmann::json& j, bool (&out)[5][4]) {
    for (int i = 0; i < 5; i++)
        for (int jj = 0; jj < 4; jj++)
            out[i][jj] = j[i][jj].get<bool>();
}

} // namespace bridge
```

**Step 2: Verify it compiles**

Add `#include "json_helpers.hpp"` to main.cpp, rebuild.

**Step 3: Commit**

```bash
git add bridge/cpp/src/json_helpers.hpp
git commit -m "feat(bridge/cpp): add JSON helper functions for 5x4 joint arrays"
```

---

### Task 3: HandBridge Class - Core Structure

**Files:**
- Create: `bridge/cpp/src/hand_bridge.hpp`
- Create: `bridge/cpp/src/hand_bridge.cpp`

**Step 1: Write hand_bridge.hpp**

```cpp
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <zenoh.hxx>

#include <wujihandcpp/device/hand.hpp>
#include <wujihandcpp/device/controller.hpp>
#include <wujihandcpp/filter/low_pass.hpp>

namespace bridge {

class HandBridge {
public:
    HandBridge(wujihandcpp::device::Hand& hand,
               std::string serial_number,
               double pub_rate = 50.0);
    ~HandBridge();

    // Non-copyable, non-movable
    HandBridge(const HandBridge&) = delete;
    HandBridge& operator=(const HandBridge&) = delete;

    void start();
    void stop();

private:
    // Zenoh key helper
    std::string key(const std::string& suffix) const;

    // Sanitize SN (replace '.' with '_')
    static std::string sanitize_sn(const std::string& sn);

    // Build @capability JSON
    std::string build_capability() const;

    // Resource GET/SET dispatch
    nlohmann::json read_resource(const std::string& path);
    void write_resource(const std::string& path, const nlohmann::json& value);

    // Publisher loop
    void publish_loop(std::stop_token st);

    // Control protocol
    void handle_control(const zenoh::Query& query);

    // Members
    wujihandcpp::device::Hand& hand_;
    std::string sn_;
    std::string sanitized_sn_;
    double pub_rate_;

    // Zenoh
    std::unique_ptr<zenoh::Session> session_;
    std::optional<zenoh::LivelinessToken> alive_token_;
    std::vector<zenoh::Queryable<void>> queryables_;
    std::vector<zenoh::Publisher<void>> publishers_;

    // Realtime controller
    std::unique_ptr<wujihandcpp::device::IController> controller_;
    std::atomic<double> cutoff_freq_{1000.0};

    // Publisher thread
    std::jthread publisher_thread_;

    // Control ownership
    std::string control_owner_;
    std::mutex control_mutex_;

    // Hand access mutex (for SDO operations)
    std::mutex hand_mutex_;

    std::atomic<bool> running_{false};
};

} // namespace bridge
```

**Step 2: Write hand_bridge.cpp - constructor, destructor, key(), sanitize_sn(), start(), stop() skeleton**

```cpp
#include "hand_bridge.hpp"

#include <chrono>
#include <iostream>
#include <thread>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "json_helpers.hpp"

namespace bridge {

HandBridge::HandBridge(wujihandcpp::device::Hand& hand,
                       std::string serial_number,
                       double pub_rate)
    : hand_(hand)
    , sn_(std::move(serial_number))
    , sanitized_sn_(sanitize_sn(sn_))
    , pub_rate_(pub_rate) {
    hand_.disable_thread_safe_check();
}

HandBridge::~HandBridge() {
    stop();
}

std::string HandBridge::key(const std::string& suffix) const {
    return "wuji/" + sanitized_sn_ + "/" + suffix;
}

std::string HandBridge::sanitize_sn(const std::string& sn) {
    std::string result = sn;
    for (auto& c : result)
        if (c == '.') c = '_';
    return result;
}

void HandBridge::start() {
    if (running_.exchange(true))
        return;

    // 1. Open Zenoh session
    auto config = zenoh::Config::create_default();
    session_ = std::make_unique<zenoh::Session>(zenoh::Session::open(std::move(config)));
    spdlog::info("Zenoh session opened");

    // 2. Liveliness token
    alive_token_.emplace(session_->liveliness().declare_token(key("@alive")));
    spdlog::info("Liveliness: {}", key("@alive"));

    // 3. Status: online
    session_->put(key("@status"), zenoh::ext::serialize("online"));
    spdlog::info("Status: online");

    // 4. Start realtime controller
    using namespace wujihandcpp;
    filter::LowPass lowpass(cutoff_freq_.load());
    controller_ = hand_.realtime_controller<true>(lowpass);
    hand_.write<data::joint::Enabled>(true);
    spdlog::info("Realtime controller started (cutoff={}Hz)", cutoff_freq_.load());

    // 5. Register @capability queryable
    auto cap_json = build_capability();
    queryables_.push_back(session_->declare_queryable(
        key("@capability"),
        [this, cap_json](const zenoh::Query& query) {
            query.reply(key("@capability"), zenoh::ext::serialize(cap_json));
        }
    ));

    // 6. Register @control queryable
    queryables_.push_back(session_->declare_queryable(
        key("@control"),
        [this](const zenoh::Query& query) { handle_control(query); }
    ));

    // 7. Register resource queryables (see Task 4)
    register_resource_queryables();

    // 8. Start publisher thread
    publisher_thread_ = std::jthread([this](std::stop_token st) { publish_loop(st); });

    spdlog::info("Bridge fully started, SN: {}", sn_);
}

void HandBridge::stop() {
    if (!running_.exchange(false))
        return;

    spdlog::info("Stopping bridge...");

    // Stop publisher thread
    publisher_thread_.request_stop();
    if (publisher_thread_.joinable())
        publisher_thread_.join();

    // Release controller
    controller_.reset();

    // Disable joints
    hand_.write<wujihandcpp::data::joint::Enabled>(false);

    // Status offline
    if (session_)
        session_->put(key("@status"), zenoh::ext::serialize("offline"));

    // Cleanup Zenoh resources (RAII)
    queryables_.clear();
    publishers_.clear();
    alive_token_.reset();
    session_.reset();

    spdlog::info("Bridge stopped");
}

} // namespace bridge
```

**Step 3: Verify compilation**

Update main.cpp to include hand_bridge.hpp, rebuild.

**Step 4: Commit**

```bash
git add bridge/cpp/src/hand_bridge.hpp bridge/cpp/src/hand_bridge.cpp
git commit -m "feat(bridge/cpp): add HandBridge core class with start/stop lifecycle"
```

---

### Task 4: Resource Handlers (GET/SET)

**Files:**
- Create: `bridge/cpp/src/resource_handlers.hpp`
- Create: `bridge/cpp/src/resource_handlers.cpp`
- Modify: `bridge/cpp/src/hand_bridge.hpp` - add `register_resource_queryables()` declaration
- Modify: `bridge/cpp/src/hand_bridge.cpp` - implement `register_resource_queryables()`, `read_resource()`, `write_resource()`

**Step 1: Implement read_resource()**

Maps resource paths to C++ SDK calls. Uses `controller_->get_joint_actual_position()` for realtime data, `hand_.read<T>()` / `hand_.finger(i).joint(j).read<T>()` for SDO data.

Resource mapping (matching Python bridge exactly):

**GET resources:**
| Path | C++ API |
|------|---------|
| `input_voltage` | `hand_.read<data::hand::InputVoltage>()` |
| `temperature` | `hand_.read<data::hand::Temperature>()` |
| `handedness` | `hand_.read<data::hand::Handedness>()` |
| `firmware_version` | `hand_.read<data::hand::FirmwareVersion>()` |
| `joint/actual_position` | `controller_->get_joint_actual_position()` (atomic, no lock) |
| `joint/actual_effort` | `controller_->get_joint_actual_effort()` (atomic, no lock) |
| `joint/temperature` | `hand_.finger(i).joint(j).read<data::joint::Temperature>()` per joint |
| `joint/error_code` | `hand_.finger(i).joint(j).read<data::joint::ErrorCode>()` per joint |
| `joint/effort_limit` | `hand_.finger(i).joint(j).read<data::joint::EffortLimit>()` per joint |
| `joint/upper_limit` | `hand_.finger(i).joint(j).read<data::joint::UpperLimit>()` per joint |
| `joint/lower_limit` | `hand_.finger(i).joint(j).read<data::joint::LowerLimit>()` per joint |
| `joint/bus_voltage` | `hand_.finger(i).joint(j).read<data::joint::BusVoltage>()` per joint |

**SET resources:**
| Path | C++ API |
|------|---------|
| `joint/target_position` | `controller_->set_joint_target_position(pos)` (direct in callback) |
| `joint/control_mode` | `hand_.finger(i).joint(j).write<data::joint::ControlMode>(v)` per joint |
| `joint/enabled` | `hand_.finger(i).joint(j).write<data::joint::Enabled>(v)` per joint |
| `joint/effort_limit` | `hand_.finger(i).joint(j).write<data::joint::EffortLimit>(v)` per joint |
| `joint/reset_error` | `hand_.finger(i).joint(j).write<data::joint::ResetError>(v)` per joint |

**Note on per-joint reads:** Use `Latch` for batch async reads for efficiency:
```cpp
wujihandcpp::device::Latch latch;
for (int i = 0; i < 5; i++)
    for (int j = 0; j < 4; j++)
        hand_.finger(i).joint(j).read_async<data::joint::Temperature>(latch);
latch.wait();
// Then get cached values
for (int i = 0; i < 5; i++)
    for (int j = 0; j < 4; j++)
        result[i][j] = hand_.finger(i).joint(j).get<data::joint::Temperature>();
```

**Step 2: Implement write_resource()**

For `joint/target_position`, directly call `controller_->set_joint_target_position()` from the Zenoh callback (no lock needed - it writes to atomic PDO buffer).

For other SET resources, acquire `hand_mutex_` and use SDO writes per-joint.

**Step 3: Implement register_resource_queryables()**

For each resource definition, create a queryable. The callback checks payload: empty = GET, non-empty = SET. SET requires control ownership check.

```cpp
void HandBridge::register_resource_queryables() {
    struct ResourceDef {
        std::string path;
        bool can_get;
        bool can_set;
        bool can_sub;
    };

    static const std::vector<ResourceDef> defs = {
        {"input_voltage",        true,  false, false},
        {"temperature",          true,  false, false},
        {"handedness",           true,  false, false},
        {"firmware_version",     true,  false, false},
        {"joint/actual_position",true,  false, true },
        {"joint/actual_effort",  true,  false, true },
        {"joint/temperature",    true,  false, false},
        {"joint/error_code",     true,  false, false},
        {"joint/effort_limit",   true,  true,  false},
        {"joint/upper_limit",    true,  false, false},
        {"joint/lower_limit",    true,  false, false},
        {"joint/bus_voltage",    true,  false, false},
        {"joint/reset_error",    false, true,  false},
        {"joint/control_mode",   false, true,  false},
        {"joint/enabled",        false, true,  false},
        {"joint/target_position",false, true,  false},
    };

    for (const auto& def : defs) {
        if (!def.can_get && !def.can_set) continue;

        queryables_.push_back(session_->declare_queryable(
            key(def.path),
            [this, def](const zenoh::Query& query) {
                handle_resource_query(query, def);
            }
        ));

        if (def.can_sub) {
            publishers_.push_back(session_->declare_publisher(key(def.path)));
        }
    }
}
```

**Step 4: Verify compilation and commit**

```bash
git add bridge/cpp/src/resource_handlers.hpp bridge/cpp/src/resource_handlers.cpp
git add bridge/cpp/src/hand_bridge.hpp bridge/cpp/src/hand_bridge.cpp
git commit -m "feat(bridge/cpp): implement resource GET/SET handlers for all 16 resources"
```

---

### Task 5: Control Protocol (@control, @capability)

**Files:**
- Modify: `bridge/cpp/src/hand_bridge.cpp` - implement `handle_control()`, `build_capability()`

**Step 1: Implement handle_control()**

Same protocol as Python bridge:
- Payload `"acquire:<requester>"` -> grant/deny
- Payload `"release:<requester>"` -> release/not_owner
- Empty payload -> return current owner

```cpp
void HandBridge::handle_control(const zenoh::Query& query) {
    auto payload = /* extract payload string */;
    std::lock_guard lock(control_mutex_);

    if (payload.starts_with("acquire:")) {
        auto requester = payload.substr(8);
        if (control_owner_.empty() || control_owner_ == requester) {
            control_owner_ = requester;
            query.reply(key("@control"), zenoh::ext::serialize("granted"));
        } else {
            query.reply(key("@control"),
                        zenoh::ext::serialize("denied:" + control_owner_));
        }
    } else if (payload.starts_with("release:")) {
        auto requester = payload.substr(8);
        if (control_owner_ == requester) {
            control_owner_.clear();
            query.reply(key("@control"), zenoh::ext::serialize("released"));
        } else {
            query.reply(key("@control"), zenoh::ext::serialize("not_owner"));
        }
    } else {
        auto owner = control_owner_.empty() ? "none" : control_owner_;
        query.reply(key("@control"), zenoh::ext::serialize(owner));
    }
}
```

**Step 2: Implement build_capability()**

Generate the same JSON structure as the Python bridge, using nlohmann/json.

**Step 3: Commit**

```bash
git add bridge/cpp/src/hand_bridge.cpp
git commit -m "feat(bridge/cpp): implement @control acquire/release and @capability protocol"
```

---

### Task 6: Publisher Loop (actual_position, actual_effort)

**Files:**
- Modify: `bridge/cpp/src/hand_bridge.cpp` - implement `publish_loop()`

**Step 1: Implement publish_loop()**

```cpp
void HandBridge::publish_loop(std::stop_token st) {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(1.0 / pub_rate_));

    auto next = clock::now();

    // publishers_[0] = actual_position, publishers_[1] = actual_effort
    while (!st.stop_requested()) {
        try {
            // actual_position
            auto pos_json = atomic_array_to_json(
                controller_->get_joint_actual_position());
            publishers_[0].put(zenoh::ext::serialize(pos_json.dump()));

            // actual_effort (may throw if firmware too old)
            try {
                auto eff_json = atomic_array_to_json(
                    controller_->get_joint_actual_effort());
                publishers_[1].put(zenoh::ext::serialize(eff_json.dump()));
            } catch (const std::runtime_error&) {
                // firmware < 1.2.0, skip effort
            }
        } catch (const std::exception& e) {
            spdlog::error("Publisher loop error: {}", e.what());
        }

        next += period;
        std::this_thread::sleep_until(next);
    }
}
```

**Step 2: Commit**

```bash
git add bridge/cpp/src/hand_bridge.cpp
git commit -m "feat(bridge/cpp): implement 100Hz publisher loop for actual_position/effort"
```

---

### Task 7: Main Entry Point

**Files:**
- Modify: `bridge/cpp/src/main.cpp`

**Step 1: Implement main.cpp**

```cpp
#include <csignal>
#include <atomic>
#include <iostream>
#include <string>

#include <spdlog/spdlog.h>

#include <wujihandcpp/device/hand.hpp>

#include "hand_bridge.hpp"

static std::atomic<bool> g_running{true};

int main(int argc, char* argv[]) {
    std::signal(SIGINT, [](int) { g_running.store(false); });
    std::signal(SIGTERM, [](int) { g_running.store(false); });

    // Simple arg parsing: --sn <serial_number> --pub-rate <hz> --log-level <level>
    std::string sn_filter;
    double pub_rate = 50.0;
    std::string log_level = "info";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--sn" && i + 1 < argc) sn_filter = argv[++i];
        else if (arg == "--pub-rate" && i + 1 < argc) pub_rate = std::stod(argv[++i]);
        else if (arg == "--log-level" && i + 1 < argc) log_level = argv[++i];
    }

    spdlog::set_level(spdlog::level::from_str(log_level));

    // Connect to hand
    spdlog::info("Connecting to hand...");
    const char* sn_ptr = sn_filter.empty() ? nullptr : sn_filter.c_str();
    wujihandcpp::device::Hand hand(sn_ptr);

    // Read product SN
    std::string sn = hand.read_product_sn();
    if (sn.empty())
        sn = "WUJIHAND_UNKNOWN";
    spdlog::info("Hand connected, SN: {}", sn);

    // Create and start bridge
    bridge::HandBridge bridge(hand, sn, pub_rate);
    bridge.start();

    spdlog::info("Bridge running. Press Ctrl+C to stop.");
    while (g_running.load())
        std::this_thread::sleep_for(std::chrono::seconds(1));

    bridge.stop();
    spdlog::info("Exiting.");
    return 0;
}
```

**Step 2: Build and verify**

```bash
cd bridge/cpp/build && cmake --build . -j$(nproc)
```

Expected: Successful build producing `wujihand_zenoh_bridge` executable.

**Step 3: Commit**

```bash
git add bridge/cpp/src/main.cpp
git commit -m "feat(bridge/cpp): implement main entry point with arg parsing and signal handling"
```

---

### Task 8: Dynamic Filter Cutoff Configuration

**Files:**
- Modify: `bridge/cpp/src/hand_bridge.hpp` - add `filter_cutoff` resource
- Modify: `bridge/cpp/src/hand_bridge.cpp` - handle SET for filter_cutoff

**Step 1: Add filter_cutoff as a SET resource**

Add to resource definitions:
```cpp
{"filter_cutoff", true, true, false}  // GET returns current cutoff, SET updates firmware filter
```

In `write_resource()`:
```cpp
if (path == "filter_cutoff") {
    float new_cutoff = value.get<float>();
    cutoff_freq_.store(new_cutoff);
    std::lock_guard lock(hand_mutex_);
    hand_.write<wujihandcpp::data::joint::PositionFilterCutoffFreq>(new_cutoff);
    spdlog::info("Filter cutoff updated to {} Hz", new_cutoff);
    return;
}
```

In `read_resource()`:
```cpp
if (path == "filter_cutoff")
    return cutoff_freq_.load();
```

**Step 2: Update @capability to include filter_cutoff**

**Step 3: Commit**

```bash
git add bridge/cpp/src/hand_bridge.hpp bridge/cpp/src/hand_bridge.cpp
git commit -m "feat(bridge/cpp): add dynamic filter cutoff frequency configuration via Zenoh"
```

---

### Task 9: Integration Test with Python Client

**Files:**
- Create: `bridge/cpp/test_bridge.py` (quick integration test using zenoh-python)

**Step 1: Write test script**

Uses the same zenoh-python client code we used to test the Python bridge:
- Discover device via liveliness
- GET @capability
- Acquire control
- SET target_position
- GET actual_position
- Release control

**Step 2: Build C++ bridge, run it, run test script in parallel**

```bash
# Terminal 1: Run C++ bridge
./build/wujihand_zenoh_bridge --log-level debug

# Terminal 2: Run test
python3 test_bridge.py
```

**Step 3: Commit**

```bash
git add bridge/cpp/test_bridge.py
git commit -m "test(bridge/cpp): add integration test script for C++ Zenoh bridge"
```

---

## Implementation Notes

### zenoh-cpp API Caveats

- zenoh-cpp API may differ between versions. The plan uses `zenoh::Session::open()`, `session.declare_queryable()`, `session.declare_publisher()`, `session.liveliness().declare_token()`. Consult zenoh-cpp examples at build time if API has changed.
- `zenoh::ext::serialize()` may not exist in all versions. Alternative: use `zenoh::Bytes` directly.
- Query payload extraction: `query.get_payload()` or similar. Check zenoh-cpp docs.

### Thread Safety

- `controller_->set_joint_target_position()` is thread-safe (writes to atomic PDO buffer) - can be called from Zenoh callback directly.
- `controller_->get_joint_actual_position()` is thread-safe (reads atomic array) - can be called from publisher thread without lock.
- All SDO operations (`hand_.read<T>()`, `hand_.write<T>()`) must be protected by `hand_mutex_`.
- `control_owner_` protected by `control_mutex_`.

### Firmware Compatibility

- `Hand` constructor auto-detects firmware and sets `feature_firmware_filter_`.
- For new firmware (>= 6.4.0-J): `realtime_controller<true>()` returns `CompatibleControllerOperator` - firmware does filtering, `PositionFilterCutoffFreq` is the knob.
- For old firmware: `realtime_controller<true>()` creates `FilteredController` - software filtering at cutoff freq.
- Either way, the API is identical via `IController` interface.
