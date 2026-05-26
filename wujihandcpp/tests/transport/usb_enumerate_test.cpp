#include <cstdint>

#include <gtest/gtest.h>

#include "wujihandcpp/transport/usb_enumerate.hpp"

using wujihandcpp::transport::list_matching_serial_numbers;

// Use a VID/PID combo unlikely to ever appear on a CI host. We cannot use the
// real Wujihand VID (0x0483) because that's a real ST vendor id and CI hosts
// may have an ST-Link or similar device plugged in.
TEST(UsbEnumerate, NoMatchingDeviceReturnsEmpty) {
    auto sns = list_matching_serial_numbers(/*vid=*/0xFFFF, /*pid=*/0x7FFF);
    EXPECT_TRUE(sns.empty());
}

TEST(UsbEnumerate, VidOnlyFilterDoesNotCrash) {
    auto sns = list_matching_serial_numbers(/*vid=*/0xFFFF, /*pid=*/-1);
    EXPECT_TRUE(sns.empty());
}
