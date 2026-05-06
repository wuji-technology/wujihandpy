#pragma once

// Tactile API is Linux-only — see wujihandcpp/data/tactile.hpp for rationale.
#if defined(__linux__)

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#include "wujihandcpp/data/tactile.hpp"
#include "wujihandcpp/data/tactile_device.hpp"
#include "wujihandcpp/protocol/tactile_command.hpp"
#include "wujihandcpp/utility/api.hpp"

namespace wujihandcpp {
namespace tactile {

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
///   - is_connected() and get_handedness() are thread-safe.
///   - read_frame() and start_streaming() are mutually exclusive.
///   - disconnect() can be called from any thread; it stops streaming.
///   - After disconnect, connect() can be called again to reconnect.
class WUJIHANDCPP_API Board {
public:
    /// Construct a Board.
    /// @param serial_number  If non-null, match this USB serial number.
    ///                       If null, use the first device with PID=0x5700.
    explicit Board(const char* serial_number = nullptr);
    ~Board();

    Board(const Board&) = delete;
    Board& operator=(const Board&) = delete;

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
    Frame read_frame(uint32_t timeout_ms = 100);

    // -- Streaming --

    /// Frame callback. Invoked on the streaming consumer thread. Must not
    /// block — long work should be dispatched to a worker queue.
    using FrameCallback = std::function<void(const Frame&)>;

    /// Disconnect callback. Fired exactly once when the device drops off
    /// the bus, on the demuxer reader thread. Must not block. Calling
    /// disconnect() from inside this callback is supported (the reader
    /// thread self-detaches).
    using DisconnectCallback = std::function<void()>;

    /// Register a disconnect callback (replaces any prior one).
    /// Pass an empty std::function to clear. May be called before connect().
    void set_disconnect_callback(DisconnectCallback callback);

    /// Start continuous frame reading on an internal thread. The frame
    /// callback is invoked for every frame. On USB disconnect, the
    /// disconnect callback (if registered) fires and streaming stops.
    /// @throws std::logic_error if already streaming.
    void start_streaming(FrameCallback callback);

    /// Stop streaming. Joins the consumer thread normally; if called from
    /// inside the consumer (e.g. from a frame callback), the thread is
    /// detached and unwinds asynchronously.
    void stop_streaming();

    // -- Identity (spec §3.1) --

    /// Spec §3.1.1 — serial / hw_revision / fw_version from device-resident TBIM.
    DeviceInfo get_device_info();

    /// Spec §3.1.2 — git short SHA of the running firmware build.
    FwBuild get_fw_build();

    /// Spec §3.1.3 — handedness from device-resident TBIM (does not require streaming).
    Handedness get_handedness();

    // -- Diagnostics (spec §3.2) --

    /// Spec §3.2.1 — uptime / counters snapshot.
    Diagnostics get_diagnostics();

    /// Non-blocking variant of get_diagnostics(). If the SDK command channel
    /// is currently busy (another command in flight on a different thread),
    /// returns false WITHOUT queueing or blocking. Otherwise behaves like
    /// get_diagnostics(): on success writes into `out` and returns true; on
    /// timeout / disconnect / non-Ok status throws as get_diagnostics() does.
    ///
    /// Intended for periodic pollers (e.g. a ROS diagnostics timer) that
    /// must yield to higher-priority caller-issued commands instead of
    /// queueing behind them on the per-channel serializer.
    bool try_get_diagnostics(Diagnostics& out);

    /// Spec §3.2.2 — zero the four diagnostic counters.
    void reset_counters();

    // -- Lifecycle (spec §3.3) --

    /// Spec §3.3.1 — toggle the data-frame stream on/off.
    void set_streaming(bool enable);

    /// Spec §3.3.2 — request a soft reset; the device will re-enumerate.
    /// SDK handle becomes invalid after this returns; caller must reconnect.
    void reset_device();

    /// Spec §3.3.3 — jump to bootloader (PID 0x5701) for OTA. The `magic`
    /// argument must equal `tactile::BOOTLOADER_MAGIC`; any other value
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
    DeviceTime get_device_time();

    /// Spec §3.5.2 — exchange host UTC nanoseconds for the device clock at
    /// the same moment; caller stores the pair to derive UTC from
    /// subsequent frames' `timestamp_ms`.
    SyncResult sync_host_epoch(uint64_t host_unix_ns);

private:
    struct Impl;
    // shared_ptr so the streaming-thread lambda can hold its own ref.
    // When the board is destroyed from inside a frame callback running on
    // the streaming thread, disconnect() detaches instead of self-joining
    // and the thread keeps Impl alive until it unwinds.
    std::shared_ptr<Impl> impl_;
};

}  // namespace tactile
}  // namespace wujihandcpp

#endif  // defined(__linux__)
