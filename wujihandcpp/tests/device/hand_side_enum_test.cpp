#include <cstdint>

#include <gtest/gtest.h>

#include "wujihandcpp/device/hand.hpp"

// Firmware convention: 0 = Right, 1 = Left (see docs/external/en/api-reference.mdx:228).
// These enum values are load-bearing — changing them flips probe_handedness'
// comparison result. The opposite convention used by tactile::Handedness does
// NOT apply here; Hand::Side is the dexterous-hand side, not the tactile-glove
// side.
TEST(HandSideEnum, ValuesMatchFirmwareConvention) {
    EXPECT_EQ(static_cast<uint8_t>(wujihandcpp::device::Hand::Side::Right), 0);
    EXPECT_EQ(static_cast<uint8_t>(wujihandcpp::device::Hand::Side::Left), 1);
}
