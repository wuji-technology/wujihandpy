#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <array>

namespace wujihandcpp::protocol {

struct TactileFrame {
    uint8_t handedness;   // 0=left, 1=right
    int16_t data[24][32]; // raw pressure values (LE)
    uint16_t sequence;
    uint32_t timestamp_ms;
};

class TactileParser {
public:
    static constexpr size_t FRAME_SIZE = 1550;

    /// Feed raw bytes from USB CDC. Returns true when a complete valid frame is parsed.
    /// USB CDC delivers data in 64-byte chunks; multiple calls may be needed.
    /// Feed raw bytes. Returns number of complete frames parsed (0 if none).
    /// frame() returns the last parsed frame.
    int feed(const std::byte* data, size_t length) {
        int frames_parsed = 0;

        for (size_t i = 0; i < length; ++i) {
            auto byte = static_cast<uint8_t>(data[i]);

            switch (state_) {
            case State::SYNC_AA:
                if (byte == 0xAA) {
                    buf_[0] = byte;
                    buf_pos_ = 1;
                    state_ = State::SYNC_55;
                }
                break;

            case State::SYNC_55:
                if (byte == 0x55) {
                    buf_[1] = byte;
                    buf_pos_ = 2;
                    state_ = State::ACCUMULATE;
                } else if (byte == 0xAA) {
                    // Stay in SYNC_55 — could be start of new header
                    buf_[0] = byte;
                    buf_pos_ = 1;
                } else {
                    state_ = State::SYNC_AA;
                }
                break;

            case State::ACCUMULATE:
                buf_[buf_pos_++] = byte;
                if (buf_pos_ == FRAME_SIZE) {
                    if (validate_and_extract()) {
                        frames_parsed++;
                    }
                    state_ = State::SYNC_AA;
                }
                break;
            }
        }

        return frames_parsed;
    }

    /// Get the last successfully parsed frame. Only valid after feed() returns true.
    const TactileFrame& frame() const { return frame_; }

private:
    enum class State : uint8_t { SYNC_AA, SYNC_55, ACCUMULATE };

    // Read u16 little-endian (portable, avoids UB and endian issues)
    static uint16_t read_u16le(const uint8_t* p) {
        return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    }

    // Read u32 little-endian
    static uint32_t read_u32le(const uint8_t* p) {
        return static_cast<uint32_t>(p[0])
             | (static_cast<uint32_t>(p[1]) << 8)
             | (static_cast<uint32_t>(p[2]) << 16)
             | (static_cast<uint32_t>(p[3]) << 24);
    }

    // Read i16 little-endian
    static int16_t read_i16le(const uint8_t* p) {
        return static_cast<int16_t>(read_u16le(p));
    }

    bool validate_and_extract() {
        // Verify length field: bytes [2..4] = 1550 (u16 LE)
        if (read_u16le(&buf_[2]) != static_cast<uint16_t>(FRAME_SIZE))
            return false;

        // CRC16-CCITT over bytes [2..1548)
        uint16_t computed_crc = crc16_ccitt(&buf_[2], 1546);
        if (computed_crc != read_u16le(&buf_[1548]))
            return false;

        // Extract fields (all little-endian)
        frame_.handedness = buf_[4];

        // Tactile data: bytes [6..1542), 24×32 i16 LE
        for (int r = 0; r < 24; ++r)
            for (int c = 0; c < 32; ++c)
                frame_.data[r][c] = read_i16le(&buf_[6 + (r * 32 + c) * 2]);

        // Sequence: bytes [1542..1544), u16 LE
        frame_.sequence = read_u16le(&buf_[1542]);

        // Timestamp: bytes [1544..1548), u32 LE
        frame_.timestamp_ms = read_u32le(&buf_[1544]);

        return true;
    }

    static uint16_t crc16_ccitt(const uint8_t* data, size_t length) {
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < length; ++i) {
            crc ^= static_cast<uint16_t>(data[i]) << 8;
            for (int j = 0; j < 8; ++j) {
                if (crc & 0x8000)
                    crc = (crc << 1) ^ 0x1021;
                else
                    crc <<= 1;
            }
        }
        return crc;
    }

    State state_ = State::SYNC_AA;
    std::array<uint8_t, FRAME_SIZE> buf_{};
    size_t buf_pos_ = 0;
    TactileFrame frame_{};
};

} // namespace wujihandcpp::protocol
