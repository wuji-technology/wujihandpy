#include "wujihandcpp/device/hand.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "logging/logging.hpp"
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

void unregister_hand_sn(const std::string& sn) noexcept {
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
    // would produce "saw 0 device(s)" without any hint about why, so handle it
    // here and throw a dedicated message naming the held SNs.
    if (serials.empty() && !skipped.empty()) {
        const char* side_str = (side == Side::Left) ? "left" : "right";
        std::string msg = std::string("No available ") + side_str + " hand found; "
            + std::to_string(skipped.size())
            + " matching device(s) already opened by this program:";
        for (const auto& sn : skipped)
            msg += " " + sn;
        msg += "; release the existing Hand or use serial_number for the other device";
        throw ConnectionError(msg);
    }

    // Dual-channel reporting strategy:
    //   - logger.warn carries the full forensic detail (SDO index in hex,
    //     original exception what()), aimed at support engineers reading
    //     ~/.wuji/log/ after the fact.
    //   - ProbeResult.failure_reason holds a short user-facing phrase that
    //     ends up in the exception message, aimed at the caller acting on
    //     the failure ("which devices were ignored, why").
    std::vector<detail::ProbeResult> results;
    results.reserve(serials.size());
    auto& logger = logging::get_logger();
    for (const auto& sn : serials) {
        try {
            // storage_unit_count=0 keeps the Handler lightweight: USB claim +
            // start/stop only, no storage allocation, no init SDO traffic.
            protocol::Handler probe(vid, pid, sn.c_str(), /*storage_unit_count=*/0);
            probe.start_transmit_receive();
            auto bytes = probe.raw_sdo_read(
                data::hand::Handedness::index, data::hand::Handedness::sub_index,
                std::chrono::milliseconds{200});
            if (bytes.empty()) {
                logger.warn(
                    "handedness probe {}: empty SDO 0x{:04x} response", sn,
                    data::hand::Handedness::index);
                results.push_back({sn, false, 0, "no response"});
            } else {
                results.push_back({sn, true, bytes[0], ""});
            }
        } catch (const TimeoutError& e) {
            logger.warn(
                "handedness probe {}: timeout reading SDO 0x{:04x} ({})", sn,
                data::hand::Handedness::index, e.what());
            results.push_back({sn, false, 0, "no response"});
        } catch (const ConnectionError& e) {
            logger.warn("handedness probe {}: USB connection failed ({})", sn, e.what());
            results.push_back({sn, false, 0, "connection failed"});
        }
    }

    auto [matches, diagnostic] = detail::select_side_matched(side, results);
    if (matches.size() == 1)
        return matches[0];

    // Surface in-process holds so the user understands why a candidate
    // wasn't probed (especially when results is empty because all matches
    // were held).
    if (!skipped.empty()) {
        diagnostic += "; already opened by this program:";
        for (const auto& sn : skipped)
            diagnostic += " " + sn;
    }
    throw ConnectionError(diagnostic);
}

} // namespace wujihandcpp::device
