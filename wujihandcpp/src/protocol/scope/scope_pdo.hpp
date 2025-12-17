#pragma once

// TPDO_SCOPE_C12 (0xE2) protocol definitions
// Only included when WUJI_SCOPE_DEBUG is defined

#include "protocol/protocol.hpp"

namespace wujihandcpp::protocol::pdo {

// 12 floats per joint for debug data
PACKED_STRUCT(ScopeC12JointData {
    float values[12];
});

// Matches firmware Spinal_TPDO_Scope_C12_T
PACKED_STRUCT(ScopeC12Result {
    uint16_t padding;
    ScopeC12JointData joint_datas[5][4];  // 5 fingers x 4 joints
    uint32_t suffix;                       // VOFA tail (0x7F800000)
});

} // namespace wujihandcpp::protocol::pdo
