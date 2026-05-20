#include "wujihandcpp/device/hand.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
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

std::mutex& registry_mu() {
    static std::mutex m;
    return m;
}

std::unordered_set<std::string>& registry_set() {
    static std::unordered_set<std::string> s;
    return s;
}

} // namespace

namespace detail {

void register_hand_sn(const std::string& sn) {
    std::lock_guard guard{registry_mu()};
    registry_set().insert(sn);
}

void unregister_hand_sn(const std::string& sn) {
    std::lock_guard guard{registry_mu()};
    registry_set().erase(sn);
}

std::vector<std::string> held_sns_snapshot() {
    std::lock_guard guard{registry_mu()};
    return {registry_set().begin(), registry_set().end()};
}

} // namespace detail

std::string Hand::probe_handedness(Side side, uint16_t vid, int32_t pid) {
    auto serials = transport::list_matching_serial_numbers(vid, pid);
    if (serials.empty())
        throw ConnectionError(
            "No device found for VID=" + format_hex16(vid) + " PID="
            + (pid < 0 ? std::string("any") : format_hex16(static_cast<uint32_t>(pid))));

    // Skip SNs already held by other Hand instances in this process — those
    // would always fail libusb_claim_interface with LIBUSB_ERROR_BUSY and
    // produce noisy error logs without contributing any new information.
    auto held = detail::held_sns_snapshot();
    std::unordered_set<std::string> held_set(held.begin(), held.end());
    std::vector<std::string> skipped;
    serials.erase(
        std::remove_if(
            serials.begin(), serials.end(),
            [&](const std::string& sn) {
                if (held_set.contains(sn)) {
                    skipped.push_back(sn);
                    return true;
                }
                return false;
            }),
        serials.end());

    // Special case: every candidate was held by this process. select_side_matched
    // would produce "saw 0 device(s)" and suggest "if firmware does not expose
    // handedness, use serial_number" — both misleading. The real cause is that
    // the caller already opened those devices.
    if (serials.empty() && !skipped.empty()) {
        const char* side_str = (side == Side::Left) ? "left" : "right";
        std::string msg = std::string("No available ") + side_str + " hand found; "
            + std::to_string(skipped.size()) + " matching device(s) already held by this process:";
        for (const auto& sn : skipped)
            msg += " " + sn;
        msg += "; release the existing Hand or use serial_number= for the other device";
        throw ConnectionError(msg);
    }

    std::vector<detail::ProbeResult> results;
    results.reserve(serials.size());
    for (const auto& sn : serials) {
        try {
            // storage_unit_count=0 keeps the Handler lightweight: USB claim +
            // start/stop only, no storage allocation, no init SDO traffic.
            protocol::Handler probe(vid, pid, sn.c_str(), /*storage_unit_count=*/0);
            probe.start_transmit_receive();
            auto bytes = probe.raw_sdo_read(
                data::hand::Handedness::index, data::hand::Handedness::sub_index,
                std::chrono::milliseconds{200});
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

    // Surface in-process holds so the user understands why a candidate
    // wasn't probed (especially when results is empty because all matches
    // were held).
    if (!skipped.empty()) {
        diagnostic += "; this process already holds:";
        for (const auto& sn : skipped)
            diagnostic += " " + sn;
    }
    throw ConnectionError(diagnostic);
}

} // namespace wujihandcpp::device
