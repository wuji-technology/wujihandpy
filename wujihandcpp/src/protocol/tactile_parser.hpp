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

    bool validate_and_extract() {
        // Verify length field: bytes [2..4] should be 1550 (u16 LE)
        uint16_t length_field;
        std::memcpy(&length_field, &buf_[2], sizeof(length_field));
        if (length_field != FRAME_SIZE)
            return false;

        // CRC16-CCITT over bytes [2..1548)
        uint16_t computed_crc = crc16_ccitt(&buf_[2], 1546);
        uint16_t received_crc;
        std::memcpy(&received_crc, &buf_[1548], sizeof(received_crc));
        if (computed_crc != received_crc)
            return false;

        // Extract fields
        frame_.handedness = buf_[4];

        // Tactile data: bytes [6..1542), 24×32 i16 LE
        std::memcpy(frame_.data, &buf_[6], 24 * 32 * sizeof(int16_t));

        // Sequence: bytes [1542..1544)
        std::memcpy(&frame_.sequence, &buf_[1542], sizeof(frame_.sequence));

        // Timestamp: bytes [1544..1548)
        std::memcpy(&frame_.timestamp_ms, &buf_[1544], sizeof(frame_.timestamp_ms));

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
