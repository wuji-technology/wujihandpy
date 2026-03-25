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

    static constexpr size_t OFFSET_LENGTH     = 2;
    static constexpr size_t OFFSET_HANDEDNESS = 4;
    static constexpr size_t OFFSET_DATA       = 6;
    static constexpr size_t OFFSET_SEQUENCE   = 1542;
    static constexpr size_t OFFSET_TIMESTAMP  = 1544;
    static constexpr size_t CRC_DATA_LENGTH   = 1546;
    static constexpr size_t OFFSET_CRC        = 1548;

    /// Feed raw bytes from USB CDC.
    /// USB CDC delivers data in 64-byte chunks; multiple calls may be needed.
    /// Returns number of complete frames parsed (0 if none).
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
                        state_ = State::SYNC_AA;
                    } else {
                        // Validation failed — resync: scan buffer for next 0xAA55
                        resync_after_failure();
                    }
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

    /// Resync after validation failure: scan buffer for next 0xAA55 header.
    void resync_after_failure() {
        // Search buf_[1..FRAME_SIZE) for next sync sequence
        for (size_t j = 1; j < FRAME_SIZE - 1; ++j) {
            if (buf_[j] == 0xAA && buf_[j + 1] == 0x55) {
                // Found potential header — shift remaining data to start
                size_t remaining = FRAME_SIZE - j;
                std::memmove(buf_.data(), &buf_[j], remaining);
                buf_pos_ = remaining;
                state_ = State::ACCUMULATE;
                return;
            }
        }
        // Check if last byte is 0xAA (possible start of next header)
        if (buf_[FRAME_SIZE - 1] == 0xAA) {
            buf_[0] = 0xAA;
            buf_pos_ = 1;
            state_ = State::SYNC_55;
        } else {
            state_ = State::SYNC_AA;
        }
    }

    bool validate_and_extract() {
        // Verify length field: bytes [OFFSET_LENGTH..OFFSET_HANDEDNESS) = 1550 (u16 LE)
        if (read_u16le(&buf_[OFFSET_LENGTH]) != static_cast<uint16_t>(FRAME_SIZE))
            return false;

        // CRC16-CCITT over bytes [OFFSET_LENGTH..OFFSET_CRC)
        uint16_t computed_crc = crc16_ccitt(&buf_[OFFSET_LENGTH], CRC_DATA_LENGTH);
        if (computed_crc != read_u16le(&buf_[OFFSET_CRC]))
            return false;

        // Extract fields (all little-endian)
        frame_.handedness = buf_[OFFSET_HANDEDNESS];

        // Tactile data: bytes [OFFSET_DATA..OFFSET_SEQUENCE), 24×32 i16 LE
        for (int r = 0; r < 24; ++r)
            for (int c = 0; c < 32; ++c)
                frame_.data[r][c] = read_i16le(&buf_[OFFSET_DATA + (r * 32 + c) * 2]);

        // Sequence: bytes [OFFSET_SEQUENCE..OFFSET_TIMESTAMP), u16 LE
        frame_.sequence = read_u16le(&buf_[OFFSET_SEQUENCE]);

        // Timestamp: bytes [OFFSET_TIMESTAMP..OFFSET_CRC), u32 LE
        frame_.timestamp_ms = read_u32le(&buf_[OFFSET_TIMESTAMP]);

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
