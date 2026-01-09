#pragma once

#include <cstdint>

#include "wujihandcpp/data/helper.hpp"

namespace wujihandcpp {

namespace device {
class Hand;
class Finger;
class Joint;
}; // namespace device

namespace data {
namespace joint {

struct FirmwareVersion : ReadOnlyData<device::Joint, 0x01, 1, uint32_t> {};
struct FirmwareDate : ReadOnlyData<device::Joint, 0x01, 2, uint32_t> {};

struct ControlMode : WriteOnlyData<device::Joint, 0x02, 1, uint16_t> {};

struct SinLevel : WriteOnlyData<device::Joint, 0x05, 8, uint16_t> {};
struct PositionFilterCutoffFreq : WriteOnlyData<device::Joint, 0x05, 19, float> {};
struct TorqueSlopeLimitPerCycle : WriteOnlyData<device::Joint, 0x05, 20, float> {};

struct EffortLimit : ReadWriteData<device::Joint, 0x07, 2, double> {
    static constexpr StorageInfo info(uint32_t) {
        // Storage is uint16_t (mA), external is double (A)
        return StorageInfo{sizeof(uint16_t), index, sub_index, StorageInfo::EFFORT_LIMIT};
    }
};

// Deprecated alias for backward compatibility
using CurrentLimit [[deprecated("Use EffortLimit instead")]] = EffortLimit;

struct BusVoltage : ReadOnlyData<device::Joint, 0x0B, 8, float> {};
struct Temperature : ReadOnlyData<device::Joint, 0x0B, 9, float> {};

struct ResetError : WriteOnlyData<device::Joint, 0x0D, 4, uint16_t> {};

struct ErrorCode : ReadOnlyData<device::Joint, 0x3F, 0, uint32_t> {};

struct Enabled : WriteOnlyData<device::Joint, 0x40, 0, bool> {
    static constexpr StorageInfo info(uint32_t) {
        return StorageInfo{sizeof(uint16_t), index, sub_index, StorageInfo::CONTROL_WORD};
    }
};

namespace internal {

static constexpr bool is_reversed_joint(uint64_t i) {
    // Reverse each J1 except thumb
    return (i & 0xFF) == 0 && i != 0x0000;
}

static constexpr uint32_t position_policy(uint64_t i) {
    return is_reversed_joint(i) ? (StorageInfo::POSITION | StorageInfo::POSITION_REVERSED)
                                : (StorageInfo::POSITION);
}

} // namespace internal

struct ActualPosition : ReadOnlyData<device::Joint, 0x64, 0, double> {
    static constexpr StorageInfo info(uint32_t i) {
        return StorageInfo{sizeof(uint32_t), index, sub_index, internal::position_policy(i)};
    }
};
struct TargetPosition : WriteOnlyData<device::Joint, 0x7A, 0, double> {
    static constexpr StorageInfo info(uint32_t i) {
        return StorageInfo{sizeof(uint32_t), index, sub_index, internal::position_policy(i)};
    }
};

struct UpperLimit : ReadOnlyData<device::Joint, 0x0E, 27, double> {
    static constexpr StorageInfo info(uint32_t i) {
        return StorageInfo{
            sizeof(uint32_t), index, internal::is_reversed_joint(i) ? uint8_t(28) : sub_index,
            internal::position_policy(i)};
    }
};
struct LowerLimit : ReadOnlyData<device::Joint, 0x0E, 28, double> {
    static constexpr StorageInfo info(uint32_t i) {
        return StorageInfo{
            sizeof(uint32_t), index, internal::is_reversed_joint(i) ? uint8_t(27) : sub_index,
            internal::position_policy(i)};
    }
};

} // namespace joint
} // namespace data

} // namespace wujihandcpp
