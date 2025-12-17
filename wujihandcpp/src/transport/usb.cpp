#include <cstdint>

#include <atomic>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

#include <libusb.h>

#include "logging/logging.hpp"
#include "transport/transport.hpp"
#include "utility/cross_os.hpp"
#include "utility/final_action.hpp"
#include "utility/ring_buffer.hpp"

namespace wujihandcpp::transport {

class Usb : public ITransport {
public:
    explicit Usb(uint16_t usb_vid, int32_t usb_pid, const char* serial_number)
        : logger_(logging::get_logger())
        , free_transmit_transfers_(transmit_transfer_count_) {
        if (!usb_init(usb_vid, usb_pid, serial_number)) {
            throw std::runtime_error{"Failed to init."};
        }

        init_transmit_transfers();
        event_thread_ = std::thread{[this]() { handle_events(); }};
    }

    Usb(const Usb&) = delete;
    Usb& operator=(const Usb&) = delete;
    Usb(Usb&&) = delete;
    Usb& operator=(Usb&&) = delete;

    ~Usb() override {
        {
            std::lock_guard guard{transmit_transfer_push_mutex_};
            stop_handling_events_.store(true, std::memory_order::relaxed);
        }
        free_transmit_transfers_.pop_front_n([](TransferWrapper* wrapper) { delete wrapper; });

        libusb_release_interface(libusb_device_handle_, target_interface_);
        if constexpr (utility::is_linux())
            libusb_attach_kernel_driver(libusb_device_handle_, 0);

        // libusb_close() reliably cancels all pending transfers and invokes their callbacks,
        // avoiding race conditions present in other cancellation methods
        libusb_close(libusb_device_handle_);

        if (event_thread_.joinable())
            event_thread_.join();

        libusb_exit(libusb_context_);
    }

    std::unique_ptr<IBuffer> request_transmit_buffer() noexcept override {
        TransferWrapper* transfer = nullptr;
        {
            std::lock_guard guard{transmit_transfer_pop_mutex_};
            free_transmit_transfers_.pop_front(
                [&transfer](TransferWrapper*&& value) { transfer = value; });
        }
        if (!transfer)
            return nullptr;

        return std::unique_ptr<IBuffer>{transfer};
    };

    void transmit(std::unique_ptr<IBuffer> buffer, size_t size) override {
        if (size > max_transfer_length_)
            throw std::invalid_argument("Transmit size exceeds maximum transfer length");

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        auto& transfer = static_cast<TransferWrapper*>(buffer.get())->transfer_;
        transfer->length = static_cast<int>(size);

        int ret = libusb_submit_transfer(transfer);
        if (ret != 0) [[unlikely]] {
            throw std::runtime_error(
                std::format(
                    "Failed to submit transmit transfer: {} ({})", ret, libusb_errname(ret)));
        }

        // If success: Ownership is transferred to libusb
        std::ignore = buffer.release();
    }

    void receive(std::function<void(const std::byte*, size_t size)> callback) override {
        if (!callback)
            throw std::invalid_argument{"Callback function cannot be null"};
        if (receive_callback_)
            throw std::logic_error{"Receive function can only be called once"};

        receive_callback_ = std::move(callback);
        init_receive_transfers();
    };

private:
    class TransferWrapper : public IBuffer {
        friend class Usb;

    public:
        explicit TransferWrapper(Usb& self)
            : self_(self)
            , transfer_(self_.create_libusb_transfer()) {}

        TransferWrapper(const TransferWrapper&) = delete;
        TransferWrapper& operator=(const TransferWrapper&) = delete;
        TransferWrapper(TransferWrapper&&) = delete;
        TransferWrapper& operator=(TransferWrapper&&) = delete;

        ~TransferWrapper() override { self_.destroy_libusb_transfer(transfer_); }

        std::byte* data() noexcept override {
            return reinterpret_cast<std::byte*>(transfer_->buffer);
        }

        size_t size() const noexcept override { return max_transfer_length_; }

    private:
        Usb& self_;

        libusb_transfer* transfer_;
    };

    bool usb_init(uint16_t vendor_id, int32_t product_id, const char* serial_number) {
        int ret;

        ret = libusb_init(&libusb_context_);
        if (ret != 0) [[unlikely]] {
            logger_.error("Failed to init libusb: {} ({})", ret, libusb_errname(ret));
            return false;
        }
        utility::FinalAction exit_libusb{[this]() { libusb_exit(libusb_context_); }};

        if (!select_device(vendor_id, product_id, serial_number))
            return false;
        utility::FinalAction close_device_handle{[this]() { libusb_close(libusb_device_handle_); }};

        if constexpr (utility::is_linux()) {
            ret = libusb_detach_kernel_driver(libusb_device_handle_, target_interface_);
            if (ret != LIBUSB_ERROR_NOT_FOUND && ret != 0) [[unlikely]] {
                logger_.error("Failed to detach kernel driver: {} ({})", ret, libusb_errname(ret));
                return false;
            }
        }

        ret = libusb_claim_interface(libusb_device_handle_, target_interface_);
        if (ret != 0) [[unlikely]] {
            logger_.error("Failed to claim interface: {} ({})", ret, libusb_errname(ret));
            return false;
        }

        // Libusb successfully initialized
        close_device_handle.disable();
        exit_libusb.disable();
        return true;
    }

    bool select_device(uint16_t vendor_id, int32_t product_id, const char* serial_number) {
        libusb_device** device_list = nullptr;
        const ssize_t device_count = libusb_get_device_list(libusb_context_, &device_list);
        if (device_count < 0) {
            logger_.error(
                "Failed to get device list: {} ({})", device_count,
                libusb_errname(static_cast<int>(device_count)));
            return false;
        }

        utility::FinalAction free_device_list{
            [&device_list]() { libusb_free_device_list(device_list, 1); }};

        auto device_descriptors = new libusb_device_descriptor[device_count];
        utility::FinalAction free_device_descriptors{
            [&device_descriptors]() { delete[] device_descriptors; }};

        std::vector<libusb_device_handle*> devices_opened;

        for (ssize_t i = 0; i < device_count; i++) {
            int ret = libusb_get_device_descriptor(device_list[i], &device_descriptors[i]);
            if (ret != 0 || device_descriptors[i].bLength == 0) {
                logger_.warn(
                    "A device descriptor failed to get: {} ({})", ret, libusb_errname(ret));
                continue;
            }
            auto& descriptors = device_descriptors[i];

            if (descriptors.idVendor != vendor_id)
                continue;
            if (descriptors.iSerialNumber == 0)
                continue;
            if (product_id >= 0 && descriptors.idProduct != product_id)
                continue;

            libusb_device_handle* handle;
            ret = libusb_open(device_list[i], &handle);
            if (ret != 0)
                continue;
            utility::FinalAction close_device{[&handle]() { libusb_close(handle); }};

            if (serial_number) {
                unsigned char serial_buf[256];
                int n = libusb_get_string_descriptor_ascii(
                    handle, descriptors.iSerialNumber, serial_buf, sizeof(serial_buf) - 1);
                if (n < 0)
                    continue;
                serial_buf[n] = '\0';

                if (strcmp(reinterpret_cast<char*>(serial_buf), serial_number) != 0)
                    continue;
            }

            close_device.disable();
            devices_opened.push_back(handle);
        }

        if (devices_opened.size() != 1) {
            for (auto& device : devices_opened)
                libusb_close(device);

            logger_.error(
                "{} found with specified vendor id (0x{:04x}){}{}",
                devices_opened.size() ? std::format("{} devices", devices_opened.size()).c_str()
                                      : "No device",
                vendor_id,
                product_id >= 0 ? std::format(", product id (0x{:04x})", product_id).c_str() : "",
                serial_number ? std::format(", serial number ({})", serial_number).c_str() : "");

            int relaxing_count = print_matched_unmatched_devices(
                device_list, device_count, device_descriptors, vendor_id, product_id,
                serial_number);

            if (devices_opened.size()) {
                if (!serial_number)
                    logger_.error(
                        "To ensure correct device selection, please specify the Serial Number");
                else
                    logger_.error(
                        "Multiple devices found, which is unusual. Consider using a device "
                        "with a unique Serial Number");
            } else {
                if (relaxing_count)
                    logger_.error("Consider relaxing some filters");
            }

            return false;
        }

        libusb_device_handle_ = devices_opened[0];
        return true;
    }

    int print_matched_unmatched_devices(
        libusb_device** device_list, ssize_t device_count,
        libusb_device_descriptor* device_descriptors, uint16_t vendor_id, int32_t product_id,
        const char* serial_number) {

        int j = 0, k = 0;
        for (ssize_t i = 0; i < device_count; i++) {
            auto& descriptors = device_descriptors[i];
            bool matched = true;

            if (descriptors.idVendor != vendor_id)
                continue;
            if (descriptors.iSerialNumber == 0)
                continue;
            if (product_id >= 0 && descriptors.idProduct != product_id)
                matched = false;

            const auto device_str = std::format(
                "Device {} ({:04x}:{:04x}):", ++j, descriptors.idVendor, descriptors.idProduct);

            libusb_device_handle* handle;
            int ret = libusb_open(device_list[i], &handle);
            if (ret != 0) {
                logger_.error(
                    "{} Ignored because device could not be opened: {} ({})", device_str, ret,
                    libusb_errname(ret));
                continue;
            }
            utility::FinalAction close_device{[&handle]() { libusb_close(handle); }};

            unsigned char serial_buf[256];
            int n = libusb_get_string_descriptor_ascii(
                handle, descriptors.iSerialNumber, serial_buf, sizeof(serial_buf) - 1);
            if (n < 0) {
                logger_.error(
                    "{} Ignored because descriptor could not be read: {} ({})", device_str, n,
                    libusb_errname(n));
                continue;
            }
            serial_buf[n] = '\0';
            const char* serial_str = reinterpret_cast<char*>(serial_buf);

            if (serial_number && std::strcmp(serial_str, serial_number) != 0)
                matched = false;

            if (matched) {
                const int match_index = ++k;
                logger_.error(
                    "{} Serial Number = {} <-- Matched #{}", device_str, serial_str, match_index);
            } else {
                logger_.error("{} Serial Number = {}", device_str, serial_str);
            }
        }
        return j;
    }

    void handle_events() {
        while (active_transfers_.load(std::memory_order::relaxed)) {
            libusb_handle_events(libusb_context_);
        }
    }

    void init_transmit_transfers() {
        TransferWrapper* transmit_transfers[transmit_transfer_count_] = {};
        try {
            for (auto& wrapper : transmit_transfers) {
                wrapper = new TransferWrapper{*this};
                auto transfer = wrapper->transfer_;

                libusb_fill_bulk_transfer(
                    transfer, libusb_device_handle_, out_endpoint_,
                    new unsigned char[max_transfer_length_], 0,
                    [](libusb_transfer* transfer) {
                        auto wrapper = static_cast<TransferWrapper*>(transfer->user_data);
                        wrapper->self_.usb_transmit_complete_callback(wrapper);
                    },
                    wrapper, 0);
                transfer->flags = libusb_transfer_flags::LIBUSB_TRANSFER_FREE_BUFFER;
            }
        } catch (...) {
            for (auto& wrapper : transmit_transfers)
                delete wrapper;
            throw;
        }

        auto iter = transmit_transfers;
        free_transmit_transfers_.push_back_n(
            [&iter]() { return *iter++; }, transmit_transfer_count_);
    }

    void init_receive_transfers() {
        for (size_t i = 0; i < receive_transfer_count_; i++) {
            auto transfer = create_libusb_transfer();

            libusb_fill_bulk_transfer(
                transfer, libusb_device_handle_, in_endpoint_,
                new unsigned char[max_transfer_length_], max_transfer_length_,
                [](libusb_transfer* transfer) {
                    static_cast<Usb*>(transfer->user_data)->usb_receive_complete_callback(transfer);
                },
                this, 0);
            transfer->flags = libusb_transfer_flags::LIBUSB_TRANSFER_FREE_BUFFER;

            int ret = libusb_submit_transfer(transfer);
            if (ret != 0) [[unlikely]] {
                destroy_libusb_transfer(transfer);
                throw std::runtime_error(
                    std::format(
                        "Failed to submit receive transfer: {} ({})", ret, libusb_errname(ret)));
            }
        }
    }

    void usb_transmit_complete_callback(TransferWrapper* wrapper) {
        // Share mutex with teardown so destructor can block callbacks before draining the queue
        std::lock_guard guard{transmit_transfer_push_mutex_};

        if (stop_handling_events_.load(std::memory_order::relaxed)) [[unlikely]] {
            delete wrapper;
            return;
        }

        free_transmit_transfers_.emplace_back(wrapper);
    }

    void usb_receive_complete_callback(libusb_transfer* transfer) {
        if (stop_handling_events_.load(std::memory_order::relaxed)) [[unlikely]] {
            destroy_libusb_transfer(transfer);
            return;
        }

        if (transfer->actual_length > 0)
            receive_callback_(
                reinterpret_cast<std::byte*>(transfer->buffer), transfer->actual_length);

        int ret = libusb_submit_transfer(transfer);
        if (ret != 0) [[unlikely]] {
            if (ret == LIBUSB_ERROR_NO_DEVICE)
                logger_.error(
                    "Failed to re-submit receive transfer: Device disconnected. "
                    "Terminating...");
            else
                logger_.error(
                    "Failed to re-submit receive transfer: {} ({}). Terminating...", ret,
                    libusb_errname(ret));
            destroy_libusb_transfer(transfer);

            // TODO: Replace abrupt termination with a flag and exception-based error handling
            std::terminate();
        }
    }

    libusb_transfer* create_libusb_transfer() {
        auto transfer = libusb_alloc_transfer(0);
        if (!transfer)
            throw std::bad_alloc{};
        active_transfers_.fetch_add(1, std::memory_order::relaxed);
        return transfer;
    }

    void destroy_libusb_transfer(libusb_transfer* transfer) {
        libusb_free_transfer(transfer);
        active_transfers_.fetch_sub(1, std::memory_order::relaxed);
    }

    static constexpr const char* libusb_errname(int number) {
        switch (number) {
        case LIBUSB_ERROR_IO: return "ERROR_IO";
        case LIBUSB_ERROR_INVALID_PARAM: return "ERROR_INVALID_PARAM";
        case LIBUSB_ERROR_ACCESS: return "ERROR_ACCESS";
        case LIBUSB_ERROR_NO_DEVICE: return "ERROR_NO_DEVICE";
        case LIBUSB_ERROR_NOT_FOUND: return "ERROR_NOT_FOUND";
        case LIBUSB_ERROR_BUSY: return "ERROR_BUSY";
        case LIBUSB_ERROR_TIMEOUT: return "ERROR_TIMEOUT";
        case LIBUSB_ERROR_OVERFLOW: return "ERROR_OVERFLOW";
        case LIBUSB_ERROR_PIPE: return "ERROR_PIPE";
        case LIBUSB_ERROR_INTERRUPTED: return "ERROR_INTERRUPTED";
        case LIBUSB_ERROR_NO_MEM: return "ERROR_NO_MEM";
        case LIBUSB_ERROR_NOT_SUPPORTED: return "ERROR_NOT_SUPPORTED";
        case LIBUSB_ERROR_OTHER: return "ERROR_OTHER";
        default: return "UNKNOWN";
        }
    }

    static constexpr int target_interface_ = 0x01;

    static constexpr unsigned char out_endpoint_ = 0x01;
    static constexpr unsigned char in_endpoint_ = 0x81;

    // 增大传输缓冲区以支持 TPDO_SCOPE_C12 (需要约 978 字节)
    static constexpr int max_transfer_length_ = 2048;

    static constexpr size_t transmit_transfer_count_ = 64;
    static constexpr size_t receive_transfer_count_ = 4;

    logging::Logger& logger_;

    libusb_context* libusb_context_;
    libusb_device_handle* libusb_device_handle_;

    std::thread event_thread_;

    std::atomic<int> active_transfers_ = 0;
    std::atomic<bool> stop_handling_events_ = false;

    utility::RingBuffer<TransferWrapper*> free_transmit_transfers_;
    std::mutex transmit_transfer_pop_mutex_, transmit_transfer_push_mutex_;

    std::function<void(const std::byte*, size_t size)> receive_callback_;
};

std::unique_ptr<ITransport>
    create_usb_transport(uint16_t usb_vid, int32_t usb_pid, const char* serial_number) {
    return std::make_unique<Usb>(usb_vid, usb_pid, serial_number);
}

} // namespace wujihandcpp::transport
