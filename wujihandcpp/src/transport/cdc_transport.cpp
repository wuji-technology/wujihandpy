#include "cdc_transport.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <sstream>
#include <string>
#include <termios.h>
#include <unistd.h>

namespace wujihandcpp {
namespace cdc {

// ---------------------------------------------------------------------------
// Device discovery via /sys/class/tty
// ---------------------------------------------------------------------------

/// Read a sysfs attribute file, trimming trailing whitespace.
static std::string read_sysfs(const std::string& path) {
    std::ifstream f(path);
    std::string val;
    if (f.is_open() && std::getline(f, val)) {
        // trim trailing whitespace/newline
        while (!val.empty() && (val.back() == '\n' || val.back() == '\r' || val.back() == ' '))
            val.pop_back();
    }
    return val;
}

/// Parse a hex string (e.g. "0483") to uint16_t. Returns 0 on failure.
static uint16_t parse_hex16(const std::string& s) {
    unsigned long v = 0;
    try { v = std::stoul(s, nullptr, 16); } catch (...) {}
    return static_cast<uint16_t>(v);
}

std::vector<DeviceInfo> discover_devices(uint16_t vid, uint16_t pid) {
    std::vector<DeviceInfo> result;
    const std::string base = "/sys/class/tty";

    DIR* dir = opendir(base.c_str());
    if (!dir) return result;

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name(ent->d_name);
        if (name.rfind("ttyACM", 0) != 0) continue;

        // Walk up the device tree to find the USB device with idVendor/idProduct.
        // Typical path: /sys/class/tty/ttyACM0/device/../../ (the USB interface's parent)
        std::string device_dir = base + "/" + name + "/device";

        // The idVendor/idProduct are on the USB device node, which is the parent
        // of the USB interface node. Try "../" (USB interface parent).
        std::string usb_dev_dir = device_dir + "/..";

        std::string dev_vid = read_sysfs(usb_dev_dir + "/idVendor");
        std::string dev_pid = read_sysfs(usb_dev_dir + "/idProduct");

        if (dev_vid.empty() || dev_pid.empty()) continue;

        if (parse_hex16(dev_vid) == vid && parse_hex16(dev_pid) == pid) {
            DeviceInfo info;
            info.tty_path = "/dev/" + name;
            info.serial_number = read_sysfs(usb_dev_dir + "/serial");
            result.push_back(std::move(info));
        }
    }
    closedir(dir);

    // Sort by tty name for deterministic ordering
    std::sort(result.begin(), result.end(),
              [](const DeviceInfo& a, const DeviceInfo& b) {
                  return a.tty_path < b.tty_path;
              });
    return result;
}

// ---------------------------------------------------------------------------
// CDC serial port open
// ---------------------------------------------------------------------------

int open_cdc(const char* tty_path) {
    int fd = open(tty_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;

    // Clear O_NONBLOCK after open (we use poll() for timeout control)
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct termios tty{};
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return -1;
    }

    // Raw mode: no echo, no canonical processing, no signal chars.
    // Essential for binary protocol data over USB CDC.
    cfmakeraw(&tty);

    // No flow control
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;

    // VMIN=1, VTIME=0: read blocks until at least 1 byte available
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return -1;
    }

    // Flush any stale data in kernel buffers
    tcflush(fd, TCIOFLUSH);

    return fd;
}

// ---------------------------------------------------------------------------
// Reliable read with timeout
// ---------------------------------------------------------------------------

ssize_t read_exact(int fd, uint8_t* buf, size_t count, uint32_t timeout_ms) {
    size_t total = 0;
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(timeout_ms);

    while (total < count) {
        // Compute remaining time to enforce cumulative timeout
        int poll_ms = -1;
        if (timeout_ms > 0) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) return static_cast<ssize_t>(total);  // timeout
            poll_ms = static_cast<int>(remaining);
        }

        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, poll_ms);

        if (ret < 0) {
            if (errno == EINTR) continue;
            return -1;  // poll error
        }
        if (ret == 0) {
            // Timeout
            return static_cast<ssize_t>(total);
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return -1;  // Device disconnected or error
        }

        ssize_t n = read(fd, buf + total, count - total);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return -1;  // Read error (EIO = disconnect)
        }
        if (n == 0) {
            return -1;  // EOF = disconnect
        }
        total += static_cast<size_t>(n);
    }

    return static_cast<ssize_t>(total);
}

}  // namespace cdc
}  // namespace wujihandcpp
