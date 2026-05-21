#include <cstdint>

#include <atomic>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <libusb.h>

#include "wujihandcpp/device/latch.hpp"
#include "wujihandcpp/transport/usb_enumerate.hpp"

#include "logging/logging.hpp"
#include "transport/transport.hpp"
#include "utility/cross_os.hpp"
#include "utility/final_action.hpp"
#include "utility/ring_buffer.hpp"

namespace wujihandcpp::transport {

namespace {

constexpr const char* libusb_errname(int number) {
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

struct EnumeratedDevice {
    libusb_device_handle* handle; // already opened; caller must libusb_close
    std::string serial_number;
};

// Enumerate VID/PID-matching devices, open each, and read its iSerialNumber
// descriptor. product_id < 0 disables the PID filter. Devices that fail to open
// or whose descriptor cannot be read are silently skipped (matches the original
// Usb::select_device behavior). Throws device::ConnectionError if
// libusb_get_device_list itself fails.
std::vector<EnumeratedDevice> enumerate_matching_devices(
    libusb_context* context, uint16_t vendor_id, int32_t product_id) {
    libusb_device** device_list = nullptr;
    const ssize_t device_count = libusb_get_device_list(context, &device_list);
    if (device_count < 0)
        throw device::ConnectionError(std::format(
            "Failed to get device list: {} ({})", device_count,
            libusb_errname(static_cast<int>(device_count))));

    utility::FinalAction free_list{[&device_list]() { libusb_free_device_list(device_list, 1); }};

    std::vector<EnumeratedDevice> matched;
    matched.reserve(static_cast<size_t>(device_count));

    for (ssize_t i = 0; i < device_count; i++) {
        libusb_device_descriptor desc;
        int ret = libusb_get_device_descriptor(device_list[i], &desc);
        if (ret != 0 || desc.bLength == 0)
            continue;
        if (desc.idVendor != vendor_id)
            continue;
        if (desc.iSerialNumber == 0)
            continue;
        if (product_id >= 0 && desc.idProduct != product_id)
            continue;

        libusb_device_handle* handle = nullptr;
        ret = libusb_open(device_list[i], &handle);
        if (ret != 0)
            continue;

        unsigned char buf[256];
        int n = libusb_get_string_descriptor_ascii(
            handle, desc.iSerialNumber, buf, sizeof(buf) - 1);
        // n == 0 means the descriptor returned an empty string, which is not a
        // useful identifier — treat it the same as a read failure and skip.
        if (n <= 0) {
            libusb_close(handle);
            continue;
        }
        // Construct the std::string with explicit length rather than relying on
        // a trailing '\0' — defensive against descriptors that embed NUL bytes.
        matched.push_back(
            {handle, std::string(reinterpret_cast<char*>(buf), static_cast<size_t>(n))});
    }
    return matched;
}

} // namespace

class Usb : public ITransport {
public:
    explicit Usb(uint16_t usb_vid, int32_t usb_pid, const char* serial_number)
        : logger_(logging::get_logger())
        , free_transmit_transfers_(transmit_transfer_count_) {
        if (!usb_init(usb_vid, usb_pid, serial_number)) {
            throw device::ConnectionError{"Failed to init."};
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
        if (receive_error_.load(std::memory_order::acquire))
            return nullptr;

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
        throw_if_receive_error();
        if (size > max_transfer_length_)
            throw std::invalid_argument("Transmit size exceeds maximum transfer length");

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        auto& transfer = static_cast<TransferWrapper*>(buffer.get())->transfer_;
        transfer->length = static_cast<int>(size);

        int ret = libusb_submit_transfer(transfer);
        if (ret != 0) [[unlikely]] {
            throw device::ConnectionError(
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

    void on_error(std::function<void(const std::string& message)> callback) override {
        error_callback_ = std::move(callback);
    }

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
        std::vector<EnumeratedDevice> opened;
        try {
            opened = enumerate_matching_devices(libusb_context_, vendor_id, product_id);
        } catch (const device::ConnectionError& e) {
            logger_.error("{}", e.what());
            return false;
        }

        utility::FinalAction close_all{[&opened]() {
            for (auto& dev : opened)
                libusb_close(dev.handle);
        }};

        std::vector<EnumeratedDevice*> matched;
        for (auto& dev : opened) {
            if (serial_number && dev.serial_number != serial_number)
                continue;
            matched.push_back(&dev);
        }

        if (matched.size() != 1) {
            const auto count_str = matched.size()
                ? std::format("{} devices", matched.size())
                : std::string("No device");
            const auto pid_str = product_id >= 0
                ? std::format(", product id (0x{:04x})", product_id)
                : std::string();
            const auto sn_str = serial_number
                ? std::format(", serial number ({})", serial_number)
                : std::string();
            logger_.error(
                "{} found with specified vendor id (0x{:04x}){}{}", count_str, vendor_id, pid_str,
                sn_str);

            for (size_t i = 0; i < opened.size(); ++i) {
                auto& dev = opened[i];
                const bool is_match = !serial_number || (dev.serial_number == serial_number);
                logger_.error(
                    "Device {} : Serial Number = {}{}", i + 1, dev.serial_number,
                    is_match ? " <-- Matched" : "");
            }

            if (matched.size() > 1 && !serial_number)
                logger_.error(
                    "To ensure correct device selection, please specify the Serial Number");
            else if (matched.size() > 1)
                logger_.error(
                    "Multiple devices found with the same Serial Number, which is unusual");
            return false;
        }

        libusb_device_handle_ = matched[0]->handle;
        // Keep the chosen handle alive; close only the others.
        close_all.disable();
        for (auto& dev : opened)
            if (dev.handle != libusb_device_handle_)
                libusb_close(dev.handle);
        return true;
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
                throw device::ConnectionError(
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

            receive_error_.store(true, std::memory_order::release);
            if (error_callback_) {
                if (ret == LIBUSB_ERROR_NO_DEVICE)
                    error_callback_("Device disconnected");
                else
                    error_callback_(
                        std::format("Failed to re-submit receive transfer: {} ({})", ret,
                                    libusb_errname(ret)));
            }
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

    void throw_if_receive_error() {
        if (receive_error_.load(std::memory_order::acquire)) [[unlikely]]
            throw device::ConnectionError("Device disconnected");
    }

    static constexpr int target_interface_ = 0x01;

    static constexpr unsigned char out_endpoint_ = 0x01;
    static constexpr unsigned char in_endpoint_ = 0x81;

    static constexpr int max_transfer_length_ = 512;

    static constexpr size_t transmit_transfer_count_ = 64;
    static constexpr size_t receive_transfer_count_ = 4;

    logging::Logger& logger_;

    libusb_context* libusb_context_;
    libusb_device_handle* libusb_device_handle_;

    std::thread event_thread_;

    std::atomic<int> active_transfers_ = 0;
    std::atomic<bool> stop_handling_events_ = false;

    std::atomic<bool> receive_error_ = false;

    utility::RingBuffer<TransferWrapper*> free_transmit_transfers_;
    std::mutex transmit_transfer_pop_mutex_, transmit_transfer_push_mutex_;

    std::function<void(const std::byte*, size_t size)> receive_callback_;
    std::function<void(const std::string& message)> error_callback_;
};

std::unique_ptr<ITransport>
    create_usb_transport(uint16_t usb_vid, int32_t usb_pid, const char* serial_number) {
    return std::make_unique<Usb>(usb_vid, usb_pid, serial_number);
}

std::vector<std::string>
    list_matching_serial_numbers(uint16_t vendor_id, int32_t product_id) {
    libusb_context* context = nullptr;
    int ret = libusb_init(&context);
    if (ret != 0)
        throw device::ConnectionError(
            std::format("Failed to init libusb: {} ({})", ret, libusb_errname(ret)));
    utility::FinalAction exit_libusb{[context]() { libusb_exit(context); }};

    auto devices = enumerate_matching_devices(context, vendor_id, product_id);
    utility::FinalAction close_all{[&devices]() {
        for (auto& dev : devices)
            libusb_close(dev.handle);
    }};

    std::vector<std::string> sns;
    sns.reserve(devices.size());
    for (auto& dev : devices)
        sns.push_back(std::move(dev.serial_number));
    return sns;
}

} // namespace wujihandcpp::transport
