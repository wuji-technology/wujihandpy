#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wujihandcpp {
namespace cdc {

/// USB device info discovered from /sys/class/tty
struct DeviceInfo {
    std::string tty_path;      ///< e.g. "/dev/ttyACM0"
    std::string serial_number; ///< USB serial string (may be empty)
};

/// Scan /sys/class/tty for USB CDC ACM devices matching vid:pid.
/// @return list of matching devices, ordered by ttyACM index.
std::vector<DeviceInfo> discover_devices(uint16_t vid, uint16_t pid);

/// Open a CDC serial port in raw mode (cfmakeraw).
/// USB CDC doesn't need baud rate, but raw mode prevents the kernel
/// line discipline from mangling binary data.
/// @return file descriptor, or -1 on failure.
int open_cdc(const char* tty_path);

/// Read exactly `count` bytes, retrying on partial reads.
/// @param fd      open file descriptor
/// @param buf     destination buffer
/// @param count   bytes to read
/// @param timeout_ms  total timeout in milliseconds (0 = no timeout)
/// @return number of bytes read, or -1 on error/disconnect.
///         Returns < count if timeout expires.
ssize_t read_exact(int fd, uint8_t* buf, size_t count, uint32_t timeout_ms);

/// Write exactly `count` bytes, retrying on partial writes / EINTR.
/// @return number of bytes written (== count) on success, -1 on error/disconnect.
ssize_t write_exact(int fd, const uint8_t* buf, size_t count);

}  // namespace cdc
}  // namespace wujihandcpp
