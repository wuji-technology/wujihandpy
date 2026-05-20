#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "wujihandcpp/device/hand.hpp"

using wujihandcpp::device::detail::held_sns_snapshot;
using wujihandcpp::device::detail::register_hand_sn;
using wujihandcpp::device::detail::unregister_hand_sn;

// The registry is process-global static state, so these tests assume they
// run before any real Hand instance has been created and clean up after
// themselves.

TEST(HandRegistry, EmptyByDefault) {
    EXPECT_TRUE(held_sns_snapshot().empty());
}

TEST(HandRegistry, RegisterAndUnregister) {
    register_hand_sn("SN_A");
    register_hand_sn("SN_B");

    auto held = held_sns_snapshot();
    std::vector<std::string> sorted(held.begin(), held.end());
    std::sort(sorted.begin(), sorted.end());
    ASSERT_EQ(sorted.size(), 2u);
    EXPECT_EQ(sorted[0], "SN_A");
    EXPECT_EQ(sorted[1], "SN_B");

    unregister_hand_sn("SN_A");
    held = held_sns_snapshot();
    ASSERT_EQ(held.size(), 1u);
    EXPECT_EQ(*held.begin(), "SN_B");

    unregister_hand_sn("SN_B");
    EXPECT_TRUE(held_sns_snapshot().empty());
}

TEST(HandRegistry, UnregisterMissingIsNoop) {
    EXPECT_TRUE(held_sns_snapshot().empty());
    unregister_hand_sn("SN_NONEXISTENT");
    EXPECT_TRUE(held_sns_snapshot().empty());
}

TEST(HandRegistry, DuplicateRegisterIsIdempotent) {
    register_hand_sn("SN_X");
    register_hand_sn("SN_X");
    EXPECT_EQ(held_sns_snapshot().size(), 1u);
    unregister_hand_sn("SN_X");
    EXPECT_TRUE(held_sns_snapshot().empty());
}
