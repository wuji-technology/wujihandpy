#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <bit>
#include <memory>
#include <stdexcept>

#include <spdlog/fmt/bin_to_hex.h>

#include "logging/logging.hpp"
#include "protocol/protocol.hpp"
#include "transport/transport.hpp"

namespace wujihandcpp::protocol {

class FrameBuilder {
public:
    FrameBuilder(transport::ITransport& transport, uint8_t header_type)
        : logger_(logging::get_logger())
        , transport_(transport)
        , header_type_(header_type) {

        buffer_ = transport_.request_transmit_buffer();
        if (!buffer_)
            throw std::runtime_error("No buffer available!");
        reset_frame();
    };

    std::byte* allocate(std::size_t size) {
        const auto required = static_cast<std::ptrdiff_t>(size)
                            + static_cast<std::ptrdiff_t>(sizeof(protocol::CrcCheck));
        if (end_ - current_ <= required)
            finalize();

        if (end_ - current_ <= required)
            throw std::invalid_argument("Expected size is too long");

        auto current = current_;
        current_ += size;
        return current;
    }

    void finalize() {
        auto new_buffer = transport_.request_transmit_buffer();
        if (!new_buffer) {
            reset_frame();
            dropped_frame_count_++;
            return;
        }

        transmit_frame();
        buffer_ = std::move(new_buffer);
        reset_frame();
    }

    uint64_t dropped_frame_count() const { return dropped_frame_count_; }

private:
    void reset_frame() {
        const auto size = buffer_->size();
        assert(size % 16 == 0);
        assert(size > sizeof(protocol::Header) + sizeof(protocol::CrcCheck));

        current_ = buffer_->data();
        end_ = current_ + size;

        auto& header = *new (current_) protocol::Header{};
        header.type = header_type_;
        current_ += sizeof(header);
    }

    void transmit_frame() {
        auto begin = buffer_->data();
        auto size = current_ - begin;

        auto compressed_frame_length =
            static_cast<uint16_t>((size + sizeof(protocol::CrcCheck) - 1) / 16 + 1);
        auto padded_length = 16 * compressed_frame_length;
        std::memset(current_, 0, padded_length - size);

        auto& header = *reinterpret_cast<protocol::Header*>(begin);
        struct {
            uint16_t max_receive_window : 10;
            uint16_t frame_length       : 6;
        } description{
            // 增大接收窗口以支持 TPDO_SCOPE_C12 (需要约 968 字节, 968/4=242, 使用 256)
            .max_receive_window = 0x100, .frame_length = (uint8_t)(compressed_frame_length - 1)};
        header.description = std::bit_cast<int16_t>(description);

        logger_.trace(
            "TX [{} bytes] {:Xp}", padded_length, spdlog::to_hex(begin, begin + padded_length));

        transport_.transmit(std::move(buffer_), padded_length);
    }

    logging::Logger& logger_;

    transport::ITransport& transport_;
    const uint8_t header_type_;

    std::unique_ptr<transport::IBuffer> buffer_ = nullptr;
    std::byte *current_ = nullptr, *end_ = nullptr;

    uint64_t dropped_frame_count_ = 0;
};

} // namespace wujihandcpp::protocol