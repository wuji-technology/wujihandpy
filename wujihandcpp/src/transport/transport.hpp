#pragma once

#include <cstddef>
#include <cstdint>

#include <functional>
#include <memory>
#include <string>

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

    /// Register a callback invoked (from an internal thread) when an unrecoverable error occurs.
    /// Must be called before receive(). Not thread-safe.
    virtual void on_error(std::function<void(const std::string& message)> callback) = 0;

    /// USB iSerialNumber descriptor of the device this transport is bound to.
    /// Captured at open time. Empty string when the underlying transport has
    /// no notion of a USB SN (e.g. a CDC stream identified by a path), or
    /// when the descriptor was unreadable. Used by Hand to register itself in
    /// the process-local SN registry regardless of which ctor was invoked.
    virtual const std::string& selected_serial_number() const noexcept = 0;
};

std::unique_ptr<ITransport>
    create_usb_transport(uint16_t usb_vid, int32_t usb_pid, const char* serial_number);

} // namespace wujihandcpp::transport
