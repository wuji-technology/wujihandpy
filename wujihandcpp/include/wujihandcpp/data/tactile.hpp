#pragma once

#include <cstdint>
#include <cstring>

namespace wujihandcpp {

/// Tactile sensor board handedness
enum class TactileHandedness : uint8_t { LEFT = 0, RIGHT = 1 };

/// Parsed tactile frame from the tactile board (tboard).
///
/// Pressure values are normalized in [0.0, 1.0]:
///   0.0 = no contact, 1.0 = maximum contact.
/// NaN marks an invalid cell (zone 0 — not part of any finger or palm region);
/// callers must skip such cells via `std::isnan` / `numpy.isnan()`.
struct TactileFrame {
    TactileHandedness hand{};      ///< Left or right hand
    uint16_t sequence{};           ///< Frame counter (wraps at 65535)
    uint32_t timestamp_ms{};       ///< Milliseconds since device boot
    float pressure[24][32]{};      ///< Pressure matrix, [0.0, 1.0]; NaN = invalid cell
    bool crc_valid{};              ///< Whether CRC16 check passed
};

namespace tactile_protocol {

// Wire layout per docs/tactile-wire-protocol.md §2.1 (data frame, 3088 B fixed).
//
//   offset  size  field
//      0      2   sync 0xAA 0x55
//      2      2   length = 3088
//      4      1   hand (0=left / 1=right)
//      5      3   pad (4-byte align tactile data at offset 8)
//      8   3072   tactile data: 24 x 32 x f32 LE, [0.0, 1.0]; NaN = invalid
//   3080      2   seq u16
//   3082      4   timestamp_ms u32 (device uptime ms)
//   3086      2   crc16 (CRC-16-CCITT over bytes [2, 3086))

constexpr uint16_t FRAME_SIZE = 3088;
constexpr uint8_t HEADER_0 = 0xAA;
constexpr uint8_t HEADER_1 = 0x55;
constexpr uint16_t EXPECTED_LENGTH = 3088;

constexpr size_t OFFSET_HEADER = 0;
constexpr size_t OFFSET_LENGTH = 2;
constexpr size_t OFFSET_HAND = 4;
constexpr size_t OFFSET_PAD = 5;            // 3-byte pad
constexpr size_t OFFSET_TACTILE_DATA = 8;
constexpr size_t TACTILE_DATA_SIZE = 24 * 32 * 4;  // 3072
constexpr size_t OFFSET_SEQUENCE = 3080;
constexpr size_t OFFSET_TIMESTAMP = 3082;
constexpr size_t OFFSET_CRC = 3086;

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

/// Parse a raw 3088-byte frame into TactileFrame.
/// Returns a TactileFrame with crc_valid set accordingly.
inline TactileFrame parse_frame(const uint8_t* raw) {
    TactileFrame frame{};

    // Hand
    frame.hand = static_cast<TactileHandedness>(raw[OFFSET_HAND]);

    // Tactile data (24 x 32 f32 LE). memcpy preserves NaN bit patterns
    // and is safe regardless of source alignment.
    std::memcpy(&frame.pressure[0][0], raw + OFFSET_TACTILE_DATA, TACTILE_DATA_SIZE);

    // Sequence (little-endian u16)
    frame.sequence = static_cast<uint16_t>(
        raw[OFFSET_SEQUENCE] | (raw[OFFSET_SEQUENCE + 1] << 8));

    // Timestamp (little-endian u32)
    frame.timestamp_ms =
        static_cast<uint32_t>(raw[OFFSET_TIMESTAMP]) |
        (static_cast<uint32_t>(raw[OFFSET_TIMESTAMP + 1]) << 8) |
        (static_cast<uint32_t>(raw[OFFSET_TIMESTAMP + 2]) << 16) |
        (static_cast<uint32_t>(raw[OFFSET_TIMESTAMP + 3]) << 24);

    // CRC: firmware computes over bytes [2, 3086), skipping the sync header.
    uint16_t expected_crc = static_cast<uint16_t>(
        raw[OFFSET_CRC] | (raw[OFFSET_CRC + 1] << 8));
    uint16_t computed_crc = crc16_ccitt(raw + OFFSET_LENGTH, OFFSET_CRC - OFFSET_LENGTH);
    frame.crc_valid = (expected_crc == computed_crc);

    return frame;
}

}  // namespace tactile_protocol
}  // namespace wujihandcpp
