#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "wujihandcpp/data/tactile.hpp"
#include "wujihandcpp/utility/api.hpp"

namespace wujihandcpp {

/// Exception thrown when USB CDC connection is lost during read
class WUJIHANDCPP_API ConnectionLostError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// USB CDC reader for the WujiHand tactile sensor board (G-Board / tboard).
///
/// The board transmits 24x32 pressure frames at 120 FPS over USB CDC
/// (VID=0x0483, PID=0x5700). This class handles device discovery,
/// frame synchronization, CRC validation, and streaming.
///
/// Thread safety:
///   - is_connected() and handedness() are thread-safe.
///   - read_frame() and start_streaming() are mutually exclusive.
///   - disconnect() can be called from any thread; it stops streaming.
///   - After disconnect, destroy and re-construct to reconnect.
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

    /// Start continuous frame reading on an internal thread.
    /// The callback is invoked for every valid frame.
    /// On USB disconnect, the callback receives a zero-initialized frame
    /// (crc_valid=false), then streaming stops automatically.
    /// @throws std::logic_error if already streaming or read_frame() is active.
    void start_streaming(FrameCallback callback);

    /// Stop streaming and join the reader thread.
    void stop_streaming();

    // -- Device info --

    /// Handedness reported by the most recent frame.
    /// Only valid after at least one frame has been read.
    TactileHandedness handedness() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wujihandcpp
