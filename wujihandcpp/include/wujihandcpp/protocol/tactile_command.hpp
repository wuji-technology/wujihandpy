#pragma once

// Tactile API is Linux-only — see wujihandcpp/data/tactile.hpp for rationale.
#if defined(__linux__)

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace wujihandcpp {
namespace tactile {

/// Tactile board command IDs (spec docs/tactile-wire-protocol.md §3).
enum class Cmd : uint16_t {
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
enum class Status : uint8_t {
    Ok           = 0x00,
    BadLength    = 0x10,
    BadCrc       = 0x11,
    UnknownCmd   = 0x12,
    BadPayload   = 0x13,
};

/// Format a status as `NAME(0xHH)` for diagnostics. Unknown codes show as
/// `UNKNOWN(0xHH)` so unfamiliar values are still readable.
inline std::string to_string(Status status) {
    const char* name = nullptr;
    switch (status) {
        case Status::Ok:         name = "OK";          break;
        case Status::BadLength:  name = "BAD_LENGTH";  break;
        case Status::BadCrc:     name = "BAD_CRC";     break;
        case Status::UnknownCmd: name = "UNKNOWN_CMD"; break;
        case Status::BadPayload: name = "BAD_PAYLOAD"; break;
    }
    char hex[6];
    std::snprintf(hex, sizeof(hex), "0x%02X",
                  static_cast<unsigned>(static_cast<uint8_t>(status)));
    if (!name) return std::string("UNKNOWN(") + hex + ")";
    return std::string(name) + "(" + hex + ")";
}

/// Magic value required by `EnterBootloader` to prevent accidental triggering (spec §3.3.3).
constexpr uint32_t BOOTLOADER_MAGIC = 0xB007B007u;

/// Maximum command/response frame size including sync, length, payload, and CRC (spec §2.2).
constexpr uint16_t FRAME_MAX = 512;

/// Default host-side command timeout. Longer than the spec's 500 ms guidance
/// to tolerate observed CDC ACM stalls during 120 Hz streaming.
constexpr uint32_t DEFAULT_TIMEOUT_MS = 2000;

/// Exception thrown when a tactile command returns a non-Ok status.
class Error : public std::runtime_error {
public:
    Error(Status status, const std::string& msg)
        : std::runtime_error(msg), status_(status) {}

    Status status() const noexcept { return status_; }

private:
    Status status_;
};

/// Caller invoked a command but the SDK is not connected (or was already
/// disconnected by an unrelated path before the call started).
class NotConnectedError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// The host could not write the command bytes to the CDC fd. Usually means
/// the device dropped off the bus between connect() and the call.
class WriteFailedError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// The command went out on the wire but no response arrived within the
/// per-call timeout. The device is still considered connected; the command
/// may or may not have taken effect on the device side.
class ResponseTimeoutError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// The command went out on the wire and the device disconnected before a
/// response could be returned. For commands that intentionally tear down
/// the USB device (RESET, ENTER_BOOTLOADER) this is the success path.
class DisconnectedDuringRequestError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

}  // namespace tactile
}  // namespace wujihandcpp

#endif  // defined(__linux__)
