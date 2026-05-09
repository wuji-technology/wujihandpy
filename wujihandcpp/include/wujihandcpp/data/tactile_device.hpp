#pragma once

// Tactile API is Linux-only — see wujihandcpp/data/tactile.hpp for rationale.
#if defined(__linux__)

#include <array>
#include <cstdint>
#include <string>

namespace wujihandcpp {
namespace tactile {

/// Reply payload of `GET_DEVICE_INFO` (spec §3.1.1, 32 B on the wire).
struct DeviceInfo {
    std::string serial;                    ///< Up to 24 ASCII chars, NUL-trimmed
    std::array<uint8_t, 4> hw_revision{};  ///< {major, minor, patch, variant}
    std::array<uint8_t, 4> fw_version{};   ///< {major, minor, patch, pre}
};

/// Reply payload of `GET_FW_BUILD` (spec §3.1.2, 8 B).
struct FwBuild {
    std::string git_short_sha;             ///< Up to 8 ASCII chars, NUL-trimmed
};

/// Reply payload of `GET_DIAGNOSTICS` (spec §3.2.1, 18 B).
struct Diagnostics {
    uint32_t uptime_ms{};
    uint32_t frame_count{};
    uint32_t crc_err_count{};
    uint32_t dropout_count{};
    uint16_t usb_reset_count{};
};

/// Reply payload of `GET_DEVICE_TIME` (spec §3.5.1, 8 B).
struct DeviceTime {
    uint64_t device_monotonic_ns{};
};

/// Reply payload of `SYNC_HOST_EPOCH` (spec §3.5.2, 16 B).
struct SyncResult {
    uint64_t device_ns_at_sync{};
    uint64_t host_ns_echo{};
};

}  // namespace tactile
}  // namespace wujihandcpp

#endif  // defined(__linux__)
