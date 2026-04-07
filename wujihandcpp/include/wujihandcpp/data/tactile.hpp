#pragma once

#include <cstdint>

namespace wujihandcpp {

/// Tactile sensor board handedness
enum class TactileHandedness : uint8_t { LEFT = 0, RIGHT = 1 };

/// Parsed tactile frame from the G-Board sensor
struct TactileFrame {
    TactileHandedness hand{};      ///< Left or right hand
    uint16_t sequence{};           ///< Frame counter (wraps at 65535)
    uint32_t timestamp_ms{};       ///< Milliseconds since device boot
    int16_t pressure[24][32]{};    ///< Pressure matrix. 0 = max pressure, 2135 = no pressure.
    bool crc_valid{};              ///< Whether CRC16 check passed
};

namespace tactile_protocol {

constexpr uint16_t FRAME_SIZE = 1550;
constexpr uint8_t HEADER_0 = 0xAA;
constexpr uint8_t HEADER_1 = 0x55;
constexpr uint16_t EXPECTED_LENGTH = 1550;

constexpr size_t OFFSET_HEADER = 0;
constexpr size_t OFFSET_LENGTH = 2;
constexpr size_t OFFSET_HAND = 4;
constexpr size_t OFFSET_RESERVED = 5;
constexpr size_t OFFSET_TACTILE_DATA = 6;
constexpr size_t TACTILE_DATA_SIZE = 1536;  // 24 * 32 * 2
constexpr size_t OFFSET_SEQUENCE = 1542;
constexpr size_t OFFSET_TIMESTAMP = 1544;
constexpr size_t OFFSET_CRC = 1548;

constexpr uint16_t CRC16_POLY = 0x1021;
constexpr uint16_t CRC16_INIT = 0xFFFF;

/// Compute CRC16-CCITT over the given data
inline uint16_t crc16_ccitt(const uint8_t* data, size_t length) {
    uint16_t crc = CRC16_INIT;
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ CRC16_POLY;
            else
                crc <<= 1;
        }
    }
    return crc;
}

/// Parse a raw 1550-byte frame into TactileFrame
/// Returns a TactileFrame with crc_valid set accordingly
inline TactileFrame parse_frame(const uint8_t* raw) {
    TactileFrame frame{};

    // Hand
    frame.hand = static_cast<TactileHandedness>(raw[OFFSET_HAND]);

    // Tactile data (little-endian i16)
    const uint8_t* tdata = raw + OFFSET_TACTILE_DATA;
    for (int r = 0; r < 24; ++r) {
        for (int c = 0; c < 32; ++c) {
            size_t idx = (r * 32 + c) * 2;
            frame.pressure[r][c] =
                static_cast<int16_t>(tdata[idx] | (tdata[idx + 1] << 8));
        }
    }

    // Sequence (little-endian u16)
    frame.sequence = static_cast<uint16_t>(
        raw[OFFSET_SEQUENCE] | (raw[OFFSET_SEQUENCE + 1] << 8));

    // Timestamp (little-endian u32)
    frame.timestamp_ms =
        static_cast<uint32_t>(raw[OFFSET_TIMESTAMP]) |
        (static_cast<uint32_t>(raw[OFFSET_TIMESTAMP + 1]) << 8) |
        (static_cast<uint32_t>(raw[OFFSET_TIMESTAMP + 2]) << 16) |
        (static_cast<uint32_t>(raw[OFFSET_TIMESTAMP + 3]) << 24);

    // CRC check: computed over bytes [2, 1548) — matches firmware which skips header
    uint16_t expected_crc = static_cast<uint16_t>(
        raw[OFFSET_CRC] | (raw[OFFSET_CRC + 1] << 8));
    uint16_t computed_crc = crc16_ccitt(raw + OFFSET_LENGTH, OFFSET_CRC - OFFSET_LENGTH);
    frame.crc_valid = (expected_crc == computed_crc);

    return frame;
}

}  // namespace tactile_protocol
}  // namespace wujihandcpp
