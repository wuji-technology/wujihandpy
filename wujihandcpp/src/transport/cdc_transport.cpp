#include "cdc_transport.hpp"

#include <algorithm>
#include <dirent.h>
#include <fstream>

namespace wujihandcpp {
namespace cdc {

// Read a sysfs attribute file, trimming trailing whitespace.
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

// Parse a hex string (e.g. "0483") to uint16_t. Returns 0 on failure.
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
        // Typical path: /sys/class/tty/ttyACM0/device/../../ (the USB interface's parent).
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

    // Sort by tty name for deterministic ordering.
    std::sort(result.begin(), result.end(),
              [](const DeviceInfo& a, const DeviceInfo& b) {
                  return a.tty_path < b.tty_path;
              });
    return result;
}

}  // namespace cdc
}  // namespace wujihandcpp
