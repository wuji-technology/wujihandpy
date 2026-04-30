#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace wujihandcpp {

/// Tactile board command IDs (spec docs/tactile-wire-protocol.md §3).
enum class TactileCmd : uint16_t {
    // Identity (0x01xx)
    GetDeviceInfo   = 0x0101,
    GetFwBuild      = 0x0102,
    GetHandedness   = 0x0103,
    // Diagnostics (0x03xx)
    GetDiagnostics  = 0x0301,
    ResetCounters   = 0x0302,
    // Lifecycle (0x04xx)
    SetStreaming    = 0x0401,
    Reset           = 0x0402,
    EnterBootloader = 0x0403,
    // Configuration (0x05xx)
    GetConfig       = 0x0501,
    SetConfig       = 0x0502,
    // Time sync (0x07xx)
    GetDeviceTime   = 0x0701,
    SyncHostEpoch   = 0x0702,
};

/// Status codes returned in response frame (spec §2.5).
enum class TactileStatus : uint8_t {
    Ok           = 0x00,
    BadLength    = 0x10,
    BadCrc       = 0x11,
    UnknownCmd   = 0x12,
    BadPayload   = 0x13,
};

/// Configuration keys (spec §3.4).
enum class TactileConfigKey : uint16_t {
    StreamingEnabled = 0x0001,
    SampleRateHz     = 0x0002,
};

/// Configuration value type tags (spec §3.4).
enum class TactileConfigType : uint8_t {
    U16     = 0,
    EnumU8  = 1,
};

/// Magic value required by `EnterBootloader` to prevent accidental triggering (spec §3.3.3).
constexpr uint32_t TACTILE_BOOTLOADER_MAGIC = 0xB007B007u;

/// Maximum command/response frame size including sync, length, payload, and CRC (spec §2.2).
constexpr uint16_t TACTILE_FRAME_MAX = 512;

/// Default per-command timeout. Spec §2.4 recommends 500 ms but it is a host
/// policy, not a firmware contract. We use 2000 ms because under sustained
/// 120 Hz streaming the host-side cdc-acm path exhibits sporadic ~0.5 s
/// stalls during which queued data frames push the response past a 500 ms
/// deadline (the firmware itself responds on the order of ms — see HIL log).
constexpr uint32_t TACTILE_DEFAULT_TIMEOUT_MS = 2000;

/// Exception thrown when a tactile command returns a non-Ok status.
class TactileError : public std::runtime_error {
public:
    TactileError(TactileStatus status, const std::string& msg)
        : std::runtime_error(msg), status_(status) {}

    TactileStatus status() const noexcept { return status_; }

private:
    TactileStatus status_;
};

/// Caller invoked a command but the SDK is not connected (or was already
/// disconnected by an unrelated path before the call started).
class TactileNotConnectedError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// The host could not write the command bytes to the CDC fd. Usually means
/// the device dropped off the bus between connect() and the call.
class TactileWriteFailedError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// The command went out on the wire but no response arrived within the
/// per-call timeout. The device is still considered connected; the command
/// may or may not have taken effect on the device side.
class TactileResponseTimeoutError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// The command went out on the wire and the device disconnected before a
/// response could be returned. For commands that intentionally tear down
/// the USB device (RESET, ENTER_BOOTLOADER) this is the success path.
class TactileDisconnectedDuringRequestError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

}  // namespace wujihandcpp
