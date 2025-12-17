#pragma once

#include <cstdint>

#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "wujihandcpp/data/hand.hpp"
#include "wujihandcpp/data/helper.hpp"
#include "wujihandcpp/data/joint.hpp"
#include "wujihandcpp/device/controller.hpp"
#include "wujihandcpp/device/data_operator.hpp"
#include "wujihandcpp/device/data_tuple.hpp"
#include "wujihandcpp/device/finger.hpp"
#include "wujihandcpp/filter/low_pass.hpp"
#include "wujihandcpp/protocol/handler.hpp"
#include "wujihandcpp/utility/logging.hpp"

namespace wujihandcpp {
namespace device {

class Hand : public DataOperator<Hand> {
    friend class DataOperator;

public:
    explicit Hand(
        const char* serial_number = nullptr, int32_t usb_pid = -1, uint16_t usb_vid = 0x0483,
        uint32_t mask = 0)
        : handler_(usb_vid, usb_pid, serial_number, data_count()) {

        init_storage_info(mask);
        handler_.start_transmit_receive();

        try {
            check_firmware_version();

            if (feature_tpdo_proactively_report_)
                handler_.enable_host_heartbeat();

            write<data::joint::Enabled>(false);

            Latch latch;
            write_async<data::joint::ControlMode>(latch, feature_firmware_filter_ ? 9 : 6);

            if (feature_firmware_filter_) {
                write_async<data::hand::RPdoId>(latch, 0x01);
                write_async<data::hand::TPdoId>(latch, 0x01);
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
            throw TimeoutError("Hand initialization timed out: joint configuration incomplete");
        }
    };

    void check_firmware_version() {
        Latch latch;
        read_async<data::hand::FirmwareVersion>(latch);
        read_async<data::joint::FirmwareVersion>(latch);
        latch.wait();

        auto hand_version = data::FirmwareVersionData{read<data::hand::FirmwareVersion>()};
        if (hand_version < data::FirmwareVersionData{3, 0, 0})
            throw std::runtime_error(
                "The firmware version (" + hand_version.to_string()
                + ") is outdated. Please contact after-sales service for an upgrade.");

        auto joint_version =
            data::FirmwareVersionData{finger(0).joint(0).get<data::joint::FirmwareVersion>()};
        bool joint_version_consistent = true;
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                if (joint_version
                    != data::FirmwareVersionData{
                        finger(i).joint(j).get<data::joint::FirmwareVersion>()})
                    joint_version_consistent = false;

        bool log_full_system_version = (hand_version >= data::FirmwareVersionData{3, 1, 0, 'D'});
        if (log_full_system_version) {
            auto full_system_version =
                data::FirmwareVersionData{read<data::hand::FullSystemFirmwareVersion>()};
            if (full_system_version.major > 0) {
                std::string firmware_msg =
                    "Using firmware version: " + full_system_version.to_string();
                logging::log(logging::Level::INFO, firmware_msg.c_str(), firmware_msg.size());
            } else
                log_full_system_version = false;
        }

        if (!log_full_system_version) {
            std::string firmware_msg =
                "Using firmware version: " + hand_version.to_string() + " & ";

            if (joint_version_consistent) {
                firmware_msg += joint_version.to_string();
                logging::log(logging::Level::INFO, firmware_msg.c_str(), firmware_msg.size());
            } else {
                firmware_msg += "[Matrix]";
                logging::log(logging::Level::INFO, firmware_msg.c_str(), firmware_msg.size());

                std::string joint_firmware_msg;
                for (int i = 0; i < 5; i++) {
                    joint_firmware_msg.clear();
                    for (int j = 0; j < 4; j++) {
                        joint_firmware_msg += "  ";
                        joint_firmware_msg +=
                            data::FirmwareVersionData{
                                finger(i).joint(j).get<data::joint::FirmwareVersion>()}
                                .to_string();
                    }
                    logging::log(
                        logging::Level::INFO, joint_firmware_msg.c_str(),
                        joint_firmware_msg.size());
                }

                constexpr char warning_msg[] =
                    "Inconsistent driver board firmware version detected";
                logging::log(logging::Level::WARN, warning_msg, sizeof(warning_msg) - 1);
            }
        }

        if (joint_version_consistent && joint_version >= data::FirmwareVersionData{6, 4, 0, 'J'}) {
            feature_firmware_filter_ = true;
            constexpr char debug_msg[] = "Firmware filter enabled";
            logging::log(logging::Level::DEBUG, debug_msg, sizeof(debug_msg) - 1);
        }
        if (hand_version >= data::FirmwareVersionData{3, 2, 0, 'B'}) {
            feature_rpdo_directly_distribute_ = true;
            constexpr char debug_msg[] = "RPdo directly distribute enabled";
            logging::log(logging::Level::DEBUG, debug_msg, sizeof(debug_msg) - 1);
        }
        if (false) { // TPdo proactively report is still not ready to perform test
            feature_tpdo_proactively_report_ = true;
            constexpr char debug_msg[] = "TPdo proactively report enabled";
            logging::log(logging::Level::DEBUG, debug_msg, sizeof(debug_msg) - 1);
        }
    }

    Finger finger_thumb() { return finger(0); }
    Finger finger_index() { return finger(1); }
    Finger finger_middle() { return finger(2); }
    Finger finger_ring() { return finger(3); }
    Finger finger_little() { return finger(4); }

    Finger finger(int index) {
        if (index < 0 || index >= sub_count_)
            throw std::runtime_error("Index out of bounds! Possible values: 0, 1, 2, 3, 4.");
        return sub(index);
    }

    auto realtime_get_joint_actual_position() -> const std::atomic<double> (&)[5][4] {
        return handler_.realtime_get_joint_actual_position();
    }

    void realtime_set_joint_target_position(const double (&positions)[5][4]) {
        handler_.realtime_set_joint_target_position(positions);
    }

    template <bool enable_upstream>
    auto realtime_controller(const filter::LowPass& filter) -> std::unique_ptr<IController> {
        if (feature_firmware_filter_) {
            write<data::joint::PositionFilterCutoffFreq>(static_cast<float>(filter.cutoff_freq()));

            return std::make_unique<CompatibleControllerOperator>(*this);
        } else {
            bool last_enabled[5][4];
            save_and_enable_joints(last_enabled);
            read<data::joint::ActualPosition>();
            revert_enabled_joints(last_enabled);

            double positions[5][4];
            for (int i = 0; i < 5; i++)
                for (int j = 0; j < 4; j++)
                    positions[i][j] = finger(i).joint(j).get<data::joint::ActualPosition>();

            auto controller =
                std::make_unique<FilteredController<filter::LowPass, enable_upstream>>(
                    positions, filter);
            auto controller_operator =
                std::make_unique<FilteredControllerOperator<filter::LowPass, enable_upstream>>(
                    *this, *controller);
            attach_realtime_controller(std::move(controller), enable_upstream);

            return controller_operator;
        }
    }

    void start_latency_test() {
        bool last_enabled[5][4];
        save_and_disable_joints(last_enabled);

        {
            Latch latch;
            write_async<data::hand::RPdoId>(latch, 0xD0);
            write_async<data::hand::TPdoId>(latch, 0xD0);
            write_async<data::hand::PdoInterval>(latch, 2000);
            write_async<data::hand::PdoEnabled>(latch, 1);
            latch.wait();
        }

        revert_disabled_joints(last_enabled);
        handler_.start_latency_test();
    }

    void stop_latency_test() {
        bool last_enabled[5][4];
        save_and_disable_joints(last_enabled);

        {
            Latch latch;
            write_async<data::hand::PdoEnabled>(latch, 0);
            latch.wait();
        }

        revert_disabled_joints(last_enabled);
        handler_.stop_latency_test();
    }

#ifdef WUJI_SCOPE_DEBUG
    // Scope Mode (TPDO_SCOPE_C12)
    // Must: disable PDO -> set TPdoId -> enable PDO
    void start_scope_mode() {
        // 1. Disable PDO
        {
            Latch latch;
            write_async<data::hand::PdoEnabled>(latch, 0);
            latch.wait();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // 2. Set TPdoId = 0xE2 (multiple writes for reliability)
        for (int i = 0; i < 5; i++) {
            {
                Latch latch;
                write_async<data::hand::TPdoId>(latch, 0xE2);
                latch.wait();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // 3. Enable PDO (triggers tpdo_request_config)
        {
            Latch latch;
            write_async<data::hand::PdoEnabled>(latch, 1);
            latch.wait();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        handler_.start_scope_mode();
    }

    void stop_scope_mode() {
        handler_.stop_scope_mode();

        // 1. Disable PDO
        {
            Latch latch;
            write_async<data::hand::PdoEnabled>(latch, 0);
            latch.wait();
        }

        // 2. Restore TPdoId = 0x01
        for (int i = 0; i < 3; i++) {
            Latch latch;
            write_async<data::hand::TPdoId>(latch, 0x01);
            latch.wait();
        }

        // 3. Enable PDO
        {
            Latch latch;
            write_async<data::hand::PdoEnabled>(latch, 1);
            latch.wait();
        }
    }

    bool configure_vofa_forwarder(
        const std::string& ip, uint16_t port, uint32_t joint_mask = 0xFFFFF) {
        return handler_.configure_vofa_forwarder(ip, port, joint_mask);
    }

    void set_vofa_enabled(bool enabled) { handler_.set_vofa_enabled(enabled); }

    void set_vofa_joint_mask(uint32_t mask) { handler_.set_vofa_joint_mask(mask); }

    std::array<float, 12> get_scope_data(int finger_id, int joint_id) {
        return handler_.get_scope_data(finger_id, joint_id);
    }

    std::array<std::array<std::array<float, 12>, 4>, 5> get_all_scope_data() {
        return handler_.get_all_scope_data();
    }
#endif

    void disable_thread_safe_check() { handler_.disable_thread_safe_check(); }

    // Raw SDO operations for debugging
    // finger_id: 0-4 for fingers, -1 for Hand level
    // joint_id: 0-3 for joints (ignored when finger_id=-1)
    std::vector<std::byte> raw_sdo_read(
        int finger_id, int joint_id, uint16_t index, uint8_t sub_index,
        std::chrono::steady_clock::duration timeout = default_timeout) {
        uint16_t full_index = index + calculate_index_offset(finger_id, joint_id);
        return handler_.raw_sdo_read(full_index, sub_index, timeout);
    }

    void raw_sdo_write(
        int finger_id, int joint_id, uint16_t index, uint8_t sub_index,
        std::span<const std::byte> data,
        std::chrono::steady_clock::duration timeout = default_timeout) {
        uint16_t full_index = index + calculate_index_offset(finger_id, joint_id);
        handler_.raw_sdo_write(full_index, sub_index, data, timeout);
    }

private:
    class CompatibleControllerOperator : public IController {
    public:
        explicit CompatibleControllerOperator(Hand& hand)
            : hand_(hand) {}

        ~CompatibleControllerOperator() override = default;

        auto get_joint_actual_position() -> const std::atomic<double> (&)[5][4] override {
            return hand_.realtime_get_joint_actual_position();
        }

        void set_joint_target_position(const double (&positions)[5][4]) override {
            hand_.realtime_set_joint_target_position(positions);
        }

    private:
        Hand& hand_;
    };

    template <typename FilterT, bool upstream_enabled>
    class FilteredControllerOperator;

    template <typename FilterT>
    class FilteredControllerOperator<FilterT, false> : public IController {
    public:
        explicit FilteredControllerOperator(
            Hand& hand, FilteredController<FilterT, false>& controller)
            : hand_(hand)
            , controller_(&controller) {}

        FilteredControllerOperator(const FilteredControllerOperator&) = delete;
        FilteredControllerOperator& operator=(const FilteredControllerOperator&) = delete;

        FilteredControllerOperator(FilteredControllerOperator&& other) noexcept
            : hand_(other.hand_)
            , controller_(other.controller_) {
            other.controller_ = nullptr;
        }
        FilteredControllerOperator& operator=(FilteredControllerOperator&&) = delete;

        ~FilteredControllerOperator() override {
            if (!controller_)
                return;
            try {
                hand_.detach_realtime_controller();
            } catch (...) {
                // TODO: Add log here
            }
        }

        void set_joint_target_position(const double (&positions)[5][4]) override {
            controller_->set(positions);
        }

    private:
        Hand& hand_;
        FilteredController<FilterT, false>* controller_;
    };

    template <typename FilterT>
    class FilteredControllerOperator<FilterT, true> : public IController {
    public:
        explicit FilteredControllerOperator(
            Hand& hand, FilteredController<FilterT, true>& controller)
            : hand_(hand)
            , controller_(&controller) {}

        FilteredControllerOperator(const FilteredControllerOperator&) = delete;
        FilteredControllerOperator& operator=(const FilteredControllerOperator&) = delete;

        FilteredControllerOperator(FilteredControllerOperator&& other) noexcept
            : hand_(other.hand_)
            , controller_(other.controller_) {
            other.controller_ = nullptr;
        }
        FilteredControllerOperator& operator=(FilteredControllerOperator&&) = delete;

        ~FilteredControllerOperator() override {
            if (!controller_)
                return;
            try {
                hand_.detach_realtime_controller();
            } catch (...) {
                // TODO: Add log here
            }
        }

        auto get_joint_actual_position() -> const std::atomic<double> (&)[5][4] override {
            return controller_->get();
        }

        void set_joint_target_position(const double (&positions)[5][4]) override {
            controller_->set(positions);
        }

    private:
        Hand& hand_;
        FilteredController<FilterT, true>* controller_;
    };

    void attach_realtime_controller(
        std::unique_ptr<IRealtimeController> controller, bool enable_upstream) {
        if (!controller)
            throw std::invalid_argument("Controller pointer must not be null.");

        bool last_enabled[5][4];
        save_and_disable_joints(last_enabled);

        {
            Latch latch;
            write_async<data::joint::ControlMode>(latch, 5);
            write_async<data::hand::RPdoId>(latch, 0x01);
            if (enable_upstream)
#ifdef WUJI_SCOPE_DEBUG
                write_async<data::hand::TPdoId>(latch, 0xE2);  // Request TPDO_SCOPE_C12
#else
                write_async<data::hand::TPdoId>(latch, 0x01);  // Request TPDO_CSP
#endif
            else
                write_async<data::hand::TPdoId>(latch, 0x00);
            write_async<data::hand::PdoInterval>(latch, 2000);
            write_async<data::hand::PdoEnabled>(latch, 1);
            latch.wait();
        }

        revert_disabled_joints(last_enabled);

        handler_.attach_realtime_controller(controller.get(), enable_upstream);
        auto ignore = controller.release();
        (void)ignore;
    }

    std::unique_ptr<IRealtimeController> detach_realtime_controller() {
        bool last_enabled[5][4];
        save_and_disable_joints(last_enabled);

        {
            Latch latch;
            write_async<data::joint::ControlMode>(latch, 6);
            write_async<data::hand::PdoEnabled>(latch, 0);
            latch.wait();
        }

        revert_disabled_joints(last_enabled);

        return std::unique_ptr<IRealtimeController>{handler_.detach_realtime_controller()};
    }

    static uint16_t calculate_index_offset(int finger_id, int joint_id) {
        if (finger_id == -1)
            return 0x0000; // Hand level
        if (finger_id < -1 || finger_id > 4)
            throw std::invalid_argument("finger_id must be -1 to 4");
        if (joint_id < 0 || joint_id > 3)
            throw std::invalid_argument("joint_id must be 0 to 3");
        return static_cast<uint16_t>(0x2000 + finger_id * 0x800 + joint_id * 0x100);
    }

    void save_and_enable_joints(bool (&last_enabled)[5][4]) {
        Latch latch;
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++) {
                auto joint = finger(i).joint(j);
                last_enabled[i][j] = joint.get<data::joint::Enabled>();
                if (!last_enabled[i][j])
                    joint.write_async<data::joint::Enabled>(latch, true);
            }
        latch.wait();
    }

    void revert_enabled_joints(const bool (&last_enabled)[5][4]) {
        Latch latch;
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                if (!last_enabled[i][j])
                    finger(i).joint(j).write_async<data::joint::Enabled>(latch, false);
        latch.wait();
    }

    void save_and_disable_joints(bool (&last_enabled)[5][4]) {
        Latch latch;
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++) {
                auto joint = finger(i).joint(j);
                last_enabled[i][j] = joint.get<data::joint::Enabled>();
                if (last_enabled[i][j])
                    joint.write_async<data::joint::Enabled>(latch, false);
            }
        latch.wait();
    }

    void revert_disabled_joints(const bool (&last_enabled)[5][4]) {
        Latch latch;
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++)
                if (last_enabled[i][j])
                    finger(i).joint(j).write_async<data::joint::Enabled>(latch, true);
        latch.wait();
    }

    using Datas = DataTuple<
        data::hand::Handedness, data::hand::HostTimeoutCounter, data::hand::FirmwareVersion,
        data::hand::FirmwareDate, data::hand::FullSystemFirmwareVersion, data::hand::SystemTime,
        data::hand::Temperature, data::hand::InputVoltage, data::hand::RPdoDirectlyDistribute,
        data::hand::TPdoProactivelyReport, data::hand::PdoEnabled, data::hand::RPdoId,
        data::hand::TPdoId, data::hand::PdoInterval, data::hand::RPdoTriggerOffset,
        data::hand::TPdoTriggerOffset>;

    protocol::Handler handler_;

    bool feature_firmware_filter_ = false;
    bool feature_rpdo_directly_distribute_ = false;
    bool feature_tpdo_proactively_report_ = false;

    static constexpr uint16_t index_offset_ = 0x0000;
    static constexpr int storage_offset_ = 0;

    using Sub = Finger;
    static constexpr int sub_count_ = 5;
    Sub sub(int index) {
        return {
            handler_, uint16_t(0x2000 + index * 0x800),
            int(Datas::count + index * Sub::data_count())};
    }
};

} // namespace device
} // namespace wujihandcpp
