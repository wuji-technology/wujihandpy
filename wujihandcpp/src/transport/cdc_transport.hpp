#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wujihandcpp {
namespace cdc {

/// USB device info discovered from /sys/class/tty.
///
/// The byte-stream IO methods that used to live here (`open_cdc`,
/// `read_exact`, `write_exact`) moved to `CdcByteStream` in
/// `cdc_byte_stream.hpp` so the byte-stream lifecycle is owned by an
/// RAII class instead of being smeared across free functions and
/// caller-owned fds. Device enumeration is the only thing that does
/// not naturally belong on a per-fd object — it scans /sys before
/// any fd exists — so it stays here as a free function.
struct DeviceInfo {
    std::string tty_path;       ///< e.g. "/dev/ttyACM0"
    std::string serial_number;  ///< USB serial string (may be empty)
};

/// Scan /sys/class/tty for USB CDC ACM devices matching vid:pid.
/// @return list of matching devices, ordered by ttyACM index.
[[nodiscard]] std::vector<DeviceInfo> discover_devices(uint16_t vid, uint16_t pid);

}  // namespace cdc
}  // namespace wujihandcpp
