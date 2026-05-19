#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "wujihandcpp/device/hand.hpp"

using wujihandcpp::device::Hand;
using wujihandcpp::device::detail::ProbeResult;
using wujihandcpp::device::detail::select_side_matched;

TEST(SelectSideMatched, SingleLeftHandHit) {
    std::vector<ProbeResult> results = {{"SN_A", uint8_t{1}, ""}};
    auto [matches, msg] = select_side_matched(Hand::Side::Left, results);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0], "SN_A");
    EXPECT_TRUE(msg.empty());
}

TEST(SelectSideMatched, SingleRightHandHit) {
    std::vector<ProbeResult> results = {{"SN_A", uint8_t{0}, ""}};
    auto [matches, msg] = select_side_matched(Hand::Side::Right, results);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0], "SN_A");
}

TEST(SelectSideMatched, NoLeftHandWhenOnlyRightInScene) {
    std::vector<ProbeResult> results = {{"SN_A", uint8_t{0}, ""}};
    auto [matches, msg] = select_side_matched(Hand::Side::Left, results);
    EXPECT_TRUE(matches.empty());
    EXPECT_NE(msg.find("No left hand found"), std::string::npos);
    EXPECT_NE(msg.find("SN_A"), std::string::npos);
    EXPECT_NE(msg.find("use serial_number"), std::string::npos);
}

TEST(SelectSideMatched, TwoSameSideAmbiguous) {
    std::vector<ProbeResult> results = {
        {"SN_A", uint8_t{1}, ""}, {"SN_B", uint8_t{1}, ""}};
    auto [matches, msg] = select_side_matched(Hand::Side::Left, results);
    EXPECT_EQ(matches.size(), 2u);
    EXPECT_NE(msg.find("Multiple left hands found"), std::string::npos);
    EXPECT_NE(msg.find("SN_A"), std::string::npos);
    EXPECT_NE(msg.find("SN_B"), std::string::npos);
    EXPECT_NE(msg.find("use serial_number to disambiguate"), std::string::npos);
}

TEST(SelectSideMatched, ProbeFailuresAreReportedInDiagnostic) {
    std::vector<ProbeResult> results = {
        {"SN_A", uint8_t{0}, ""},
        {"SN_B", std::nullopt, "timeout: SDO 0x5090"}};
    auto [matches, msg] = select_side_matched(Hand::Side::Left, results);
    EXPECT_TRUE(matches.empty());
    EXPECT_NE(msg.find("probe failures"), std::string::npos);
    EXPECT_NE(msg.find("SN_B"), std::string::npos);
    EXPECT_NE(msg.find("timeout"), std::string::npos);
}

TEST(SelectSideMatched, AllProbesFailedHintsFirmwareSupport) {
    std::vector<ProbeResult> results = {{"SN_A", std::nullopt, "timeout"}};
    auto [matches, msg] = select_side_matched(Hand::Side::Left, results);
    EXPECT_TRUE(matches.empty());
    EXPECT_NE(msg.find("firmware does not expose handedness"), std::string::npos);
}
