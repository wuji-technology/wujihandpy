#pragma once

// Tactile transport is Linux-only; hide public tactile symbols on platforms
// where the CDC implementation is not built.
#if defined(__linux__)

#include <cstdint>
#include <cstring>

namespace wujihandcpp {
namespace tactile {

/// Tactile sensor board handedness
enum class Handedness : uint8_t { LEFT = 0, RIGHT = 1 };

/// Parsed tactile frame from the tactile board (tboard).
///
/// Pressure values are normalized in [0.0, 1.0]:
///   0.0 = no contact, 1.0 = maximum contact.
/// NaN marks an invalid cell (zone 0 — not part of any finger or palm region);
/// callers must skip such cells via `std::isnan` / `numpy.isnan()`.
///
/// CRC validation happens in the demuxer before the frame is delivered;
/// callers never see a bad-CRC frame.
struct Frame {
    Handedness hand{};             ///< Left or right hand
    uint16_t sequence{};           ///< Frame counter (wraps at 65535)
    uint32_t timestamp_ms{};       ///< Milliseconds since device boot
    float pressure[24][32]{};      ///< Pressure matrix, [0.0, 1.0]; NaN = invalid cell
};

namespace protocol {

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

/// Parse a raw 3088-byte frame into Frame. CRC is validated by the
/// demuxer before parse_frame() is called.
inline Frame parse_frame(const uint8_t* raw) {
    Frame frame{};

    frame.hand = static_cast<Handedness>(raw[OFFSET_HAND]);

    // memcpy preserves NaN bit patterns and is safe regardless of alignment.
    std::memcpy(&frame.pressure[0][0], raw + OFFSET_TACTILE_DATA, TACTILE_DATA_SIZE);

    frame.sequence = static_cast<uint16_t>(
        raw[OFFSET_SEQUENCE] | (raw[OFFSET_SEQUENCE + 1] << 8));

    frame.timestamp_ms =
        static_cast<uint32_t>(raw[OFFSET_TIMESTAMP]) |
        (static_cast<uint32_t>(raw[OFFSET_TIMESTAMP + 1]) << 8) |
        (static_cast<uint32_t>(raw[OFFSET_TIMESTAMP + 2]) << 16) |
        (static_cast<uint32_t>(raw[OFFSET_TIMESTAMP + 3]) << 24);

    return frame;
}

}  // namespace protocol
}  // namespace tactile
}  // namespace wujihandcpp

#endif  // defined(__linux__)
