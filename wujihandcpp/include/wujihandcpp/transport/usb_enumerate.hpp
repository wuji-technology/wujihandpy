#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wujihandcpp/utility/api.hpp"

namespace wujihandcpp::transport {

/// Enumerate hosts USB devices matching `vendor_id` (required) and optional
/// `product_id`, returning their iSerialNumber descriptor strings.
///
/// - `product_id < 0` filters by VID only; `product_id >= 0` filters by both.
/// - Devices with `iSerialNumber == 0` or whose descriptor cannot be opened /
///   read are silently skipped (same behavior as `Usb::select_device`).
/// - Order matches `libusb_get_device_list` enumeration order.
/// - Returns an empty vector when enumeration succeeded but nothing matched.
/// - Throws `device::ConnectionError` if `libusb_init` or
///   `libusb_get_device_list` fails.
WUJIHANDCPP_API std::vector<std::string>
    list_matching_serial_numbers(uint16_t vendor_id, int32_t product_id);

} // namespace wujihandcpp::transport
