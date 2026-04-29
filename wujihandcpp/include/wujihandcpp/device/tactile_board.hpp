#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "wujihandcpp/data/tactile.hpp"
#include "wujihandcpp/data/tactile_device.hpp"
#include "wujihandcpp/protocol/tactile_command.hpp"
#include "wujihandcpp/utility/api.hpp"

namespace wujihandcpp {

/// Exception thrown when USB CDC connection is lost during read
class WUJIHANDCPP_API ConnectionLostError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// USB CDC driver for the WujiHand tactile sensor board (tboard).
///
/// The board transmits 24x32 f32 pressure frames over USB CDC
/// (VID=0x0483, PID=0x5700). Default and upper sample rate is 120 Hz; the
/// effective rate is set via SET_CONFIG `sample_rate_hz` (1–120). This class
/// handles device discovery, frame synchronization, CRC validation, and
/// streaming. See `docs/tactile-wire-protocol.md` in the firmware repo for
/// the wire format and command set.
///
/// Thread safety:
///   - is_connected() and handedness() are thread-safe.
///   - read_frame() and start_streaming() are mutually exclusive.
///   - disconnect() can be called from any thread; it stops streaming.
///   - After disconnect, connect() can be called again to reconnect.
class WUJIHANDCPP_API TactileBoard {
public:
    /// Construct a TactileBoard.
    /// @param serial_number  If non-null, match this USB serial number.
    ///                       If null, use the first device with PID=0x5700.
    explicit TactileBoard(const char* serial_number = nullptr);
    ~TactileBoard();

    TactileBoard(const TactileBoard&) = delete;
    TactileBoard& operator=(const TactileBoard&) = delete;

    // -- Connection management --

    /// Open the USB CDC device (/dev/ttyACMx).
    /// @return true on success, false if device not found.
    bool connect();

    /// Close the device. Stops streaming if active. Thread-safe.
    void disconnect();

    /// @return true if the device fd is open.
    bool is_connected() const;

    // -- Frame reading --

    /// Read one tactile frame (blocking, with timeout).
    /// Performs frame synchronization and CRC validation internally.
    /// @param timeout_ms  Max wait time in milliseconds.
    /// @throws ConnectionLostError  if the USB device disconnects.
    /// @throws std::runtime_error   if timeout expires with no valid frame.
    TactileFrame read_frame(uint32_t timeout_ms = 100);

    // -- Streaming --

    /// Callback type for streaming mode.
    /// Called on the internal reader thread. Must not block.
    using FrameCallback = std::function<void(const TactileFrame&)>;

    /// Callback fired exactly once when the USB device disconnects (after
    /// either streaming or a blocking read sees ConnectionLostError).
    /// Called on the internal reader thread. Must not block.
    using DisconnectCallback = std::function<void()>;

    /// Register a disconnect callback (replaces any prior one).
    /// Pass an empty std::function to clear. May be called before connect().
    void set_disconnect_callback(DisconnectCallback callback);

    /// Start continuous frame reading on an internal thread.
    /// The frame callback is invoked for every valid frame. On USB
    /// disconnect, the disconnect callback (if registered) is invoked and
    /// streaming stops automatically.
    /// @throws std::logic_error if already streaming or read_frame() is active.
    void start_streaming(FrameCallback callback);

    /// Stop streaming and join the reader thread.
    void stop_streaming();

    // -- Identity (spec §3.1) --

    /// Spec §3.1.1 — serial / hw_revision / fw_version from device-resident TBIM.
    TactileDeviceInfo get_device_info();

    /// Spec §3.1.2 — git short SHA of the running firmware build.
    TactileFwBuild get_fw_build();

    /// Spec §3.1.3 — handedness from device-resident TBIM (does not require streaming).
    TactileHandedness get_handedness();

    // -- Diagnostics (spec §3.2) --

    /// Spec §3.2.1 — uptime / counters snapshot.
    TactileDiagnostics get_diagnostics();

    /// Spec §3.2.2 — zero the four diagnostic counters.
    void reset_counters();

    // -- Lifecycle (spec §3.3) --

    /// Spec §3.3.1 — toggle the data-frame stream on/off.
    void set_streaming(bool enable);

    /// Spec §3.3.2 — request a soft reset; the device will re-enumerate.
    /// SDK handle becomes invalid after this returns; caller must reconnect.
    void reset_device();

    /// Spec §3.3.3 — jump to bootloader (PID 0x5701) for OTA. The `magic`
    /// argument must equal `TACTILE_BOOTLOADER_MAGIC`; any other value
    /// triggers `BadPayload`. SDK handle becomes invalid after this call.
    void enter_bootloader(uint32_t magic);

    // -- Configuration (spec §3.4) --

    /// Spec §3.4 — current sample rate (always 1..120).
    uint16_t get_sample_rate_hz();

    /// Spec §3.4 — set sample rate, must be in 1..120.
    void set_sample_rate_hz(uint16_t hz);

    /// Spec §3.4 — read the streaming-enabled flag (mirrors set_streaming()).
    bool get_streaming_enabled();

    // -- Time sync (spec §3.5) --

    /// Spec §3.5.1 — device monotonic clock in nanoseconds.
    TactileDeviceTime get_device_time();

    /// Spec §3.5.2 — exchange host UTC nanoseconds for the device clock at
    /// the same moment; caller stores the pair to derive UTC from
    /// subsequent frames' `timestamp_ms`.
    TactileSyncResult sync_host_epoch(uint64_t host_unix_ns);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wujihandcpp
