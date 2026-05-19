#include "wujihandcpp/device/hand.hpp"

#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "wujihandcpp/device/latch.hpp"
#include "wujihandcpp/protocol/handler.hpp"
#include "wujihandcpp/transport/usb_enumerate.hpp"

namespace wujihandcpp::device {

namespace {

std::string format_hex16(uint32_t value) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%04x", value);
    return std::string(buf);
}

} // namespace

std::string Hand::probe_handedness(Side side, uint16_t vid, int32_t pid) {
    auto serials = transport::list_matching_serial_numbers(vid, pid);
    if (serials.empty())
        throw ConnectionError(
            "No device found for VID=" + format_hex16(vid) + " PID="
            + (pid < 0 ? std::string("any") : format_hex16(static_cast<uint32_t>(pid))));

    std::vector<detail::ProbeResult> results;
    results.reserve(serials.size());
    for (const auto& sn : serials) {
        try {
            // storage_unit_count=0 keeps the Handler lightweight: USB claim +
            // start/stop only, no storage allocation, no init SDO traffic.
            protocol::Handler probe(vid, pid, sn.c_str(), /*storage_unit_count=*/0);
            probe.start_transmit_receive();
            auto bytes = probe.raw_sdo_read(
                /*index=*/0x5090, /*sub_index=*/0, std::chrono::milliseconds{200});
            if (bytes.empty())
                results.push_back({sn, std::nullopt, "empty SDO response"});
            else
                results.push_back({sn, bytes[0], ""});
        } catch (const TimeoutError& e) {
            results.push_back({sn, std::nullopt, std::string("timeout: ") + e.what()});
        } catch (const ConnectionError& e) {
            results.push_back({sn, std::nullopt, std::string("connect: ") + e.what()});
        }
    }

    auto [matches, diagnostic] = detail::select_side_matched(side, results);
    if (matches.size() == 1)
        return matches[0];
    throw ConnectionError(diagnostic);
}

} // namespace wujihandcpp::device
