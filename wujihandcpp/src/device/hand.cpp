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

// Process-local SN registry — keeps the wujihandcpp shared library's ABI
// surface free of these helpers. Hand ctor (defined below) calls them
// directly within the library; external consumers neither see the
// declarations nor have any way to link against them.

std::mutex& registry_mu() {
    static std::mutex m;
    return m;
}

std::unordered_set<std::string>& registry_set() {
    static std::unordered_set<std::string> s;
    return s;
}

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

// Probe each VID/PID-matching USB device, read SDO 0x5090, and return the SN
// whose handedness matches `side`. Throws ConnectionError when zero/multiple
// candidates match. File-local: only the Hand(Side, ...) ctor needs it.
std::string probe_handedness(Hand::Side side, uint16_t vid, int32_t pid) {
    auto serials = transport::list_matching_serial_numbers(vid, pid);
    if (serials.empty())
        throw ConnectionError(
            "No device found for VID=" + format_hex16(vid)
            + " PID=" + (pid < 0 ? std::string("any") : format_hex16(static_cast<uint32_t>(pid))));

    // Skip SNs already held by other Hand instances in this process — those
    // would always fail libusb_claim_interface with LIBUSB_ERROR_BUSY and
    // produce noisy error logs without contributing any new information.
    auto held = held_sns_snapshot();
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
        const char* side_str = (side == Hand::Side::Left) ? "left" : "right";
        std::string msg = std::string("No available ") + side_str + " hand found; "
                        + std::to_string(skipped.size()) + " matching device(s)";
        for (const auto& sn : skipped)
            msg += " " + sn;
        msg += "; use serial_number to specify the device";
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
            } else if (bytes[0] > 1) {
                // Firmware contract is 0 = Right, 1 = Left. Anything else means
                // the field is corrupted or the firmware uses an unexpected
                // encoding — neither outcome should silently fall into the
                // "doesn't match side" branch.
                logger.warn(
                    "handedness probe {}: invalid handedness value {} (expected 0 or 1)", sn,
                    bytes[0]);
                results.push_back({sn, false, 0, "invalid handedness value"});
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
        } catch (const std::exception& e) {
            // Defensive catch-all: a single device's unexpected failure
            // (bad_alloc, libusb misbehavior, anything beyond Timeout/Connection)
            // should not stop probing of the remaining candidates.
            logger.warn("handedness probe {}: unexpected error ({})", sn, e.what());
            results.push_back({sn, false, 0, "probe error"});
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

} // namespace

// ----- Hand::SnRegistration -----------------------------------------------

Hand::SnRegistration::~SnRegistration() noexcept {
    // Guard against any future change that makes unregister_hand_sn throw:
    // throwing from a dtor causes std::terminate in C++17+.
    if (!sn.empty()) {
        try {
            unregister_hand_sn(sn);
        } catch (...) {
        }
    }
}

// ----- Hand ----------------------------------------------------------------

WUJIHANDCPP_API Hand::Hand(
    const char* serial_number, int32_t usb_pid, uint16_t usb_vid, uint32_t mask)
    : handler_(usb_vid, usb_pid, serial_number, data_count()) {

    init_storage_info(mask);
    handler_.start_transmit_receive();

    try {
        check_firmware_version();

        write<data::joint::Enabled>(false);

        Latch latch;
        write_async<data::joint::ControlMode>(latch, feature_firmware_filter_ ? 9 : 6);

        if (feature_firmware_filter_) {
            write_async<data::hand::RPdoId>(latch, 0x01);
            uint16_t tpdo_id = feature_exception_detect_ ? 0x02 : 0x01;
            write_async<data::hand::TPdoId>(latch, tpdo_id);
            write_async<data::hand::PdoInterval>(
                latch, feature_rpdo_directly_distribute_ ? 1000 : 2000);
            write_async<data::hand::PdoEnabled>(latch, 1);
        } else
            write_async<data::joint::CurrentLimit>(latch, 1000);

        if (feature_rpdo_directly_distribute_)
            write_async<data::hand::RPdoDirectlyDistribute>(latch, 1);
        if (feature_tpdo_proactively_report_)
            write_async<data::hand::TPdoProactivelyReport>(latch, 1);

        latch.wait();

    } catch (const TimeoutError&) {
        int disconnected_count = 0;
        std::string disconnected;
        for (int i = 0; i < sub_count_; i++)
            for (int j = 0; j < Finger::sub_count_; j++)
                if (finger(i).joint(j).get<data::joint::FirmwareVersion>() == 0) {
                    disconnected_count++;
                    disconnected +=
                        " finger(" + std::to_string(i) + ").joint(" + std::to_string(j) + ")";
                }

        if (disconnected_count == 0)
            throw TimeoutError("Failed to initialize hand: configuration timed out");
        else if (disconnected_count < sub_count_ * Finger::sub_count_)
            throw TimeoutError(
                "Failed to initialize hand: joint(s) not responding:" + disconnected);
        else
            throw TimeoutError("Failed to initialize hand: no response from device");
    }

    // Register the actually selected USB SN (queried from the handler — Hand
    // is friend of protocol::Handler so the private accessor is callable
    // from here). This way Hand() no-arg and Hand(serial_number=) both feed
    // the registry the same way, so a subsequent Hand(side=...) probe
    // correctly skips this device regardless of which ctor opened it.
    //
    // Done after all init succeeded — any throw above unwinds without
    // sn_guard_ ever holding a non-empty sn, so its dtor is a no-op for
    // the partially-constructed case.
    const auto& actual_sn = handler_.selected_serial_number();
    if (!actual_sn.empty()) {
        sn_guard_.sn = actual_sn;
        register_hand_sn(sn_guard_.sn);
    }
}

WUJIHANDCPP_API Hand::Hand(Side side, int32_t usb_pid, uint16_t usb_vid, uint32_t mask)
    // Lifetime note: probe_handedness(...) returns std::string by value. The
    // temporary lives until the end of the full expression — which spans the
    // entire delegated ctor call — so .c_str() is valid throughout the
    // delegated init (see [class.temporary]). The delegated ctor copies the
    // SN into sn_guard_.sn, so no dangling pointer survives this scope.
    : Hand(probe_handedness(side, usb_vid, usb_pid).c_str(), usb_pid, usb_vid, mask) {}

// Compiler-generated dtor destroys members in reverse declaration order:
// handler_ first releases libusb_claim_interface, then sn_guard_ unregisters
// the SN. = default lets the compiler emit the right destruction sequence.
WUJIHANDCPP_API Hand::~Hand() noexcept = default;

} // namespace wujihandcpp::device
