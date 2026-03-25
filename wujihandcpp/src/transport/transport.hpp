#pragma once

#include <cstddef>
#include <cstdint>

#include <functional>
#include <memory>

namespace wujihandcpp::transport {

class IBuffer {
public:
    virtual ~IBuffer() noexcept = default;

    virtual std::byte* data() noexcept = 0;

    virtual size_t size() const noexcept = 0;
};

class ITransport {
public:
    virtual ~ITransport() noexcept = default;

    virtual std::unique_ptr<IBuffer> request_transmit_buffer() noexcept = 0;

    virtual void transmit(std::unique_ptr<IBuffer> buffer, size_t size) = 0;

    virtual void receive(std::function<void(const std::byte* buffer, size_t size)> callback) = 0;
};

std::unique_ptr<ITransport>
    create_usb_transport(uint16_t usb_vid, uint16_t usb_pid, const char* serial_number);

std::unique_ptr<ITransport>
    create_usb_transport(uint16_t usb_vid, uint16_t usb_pid, const char* serial_number,
                         int interface_num, unsigned char in_ep, unsigned char out_ep);

} // namespace wujihandcpp::transport
