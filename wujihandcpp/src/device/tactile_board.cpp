#include "wujihandcpp/device/tactile_board.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>
#include <unistd.h>

#include "../transport/cdc_transport.hpp"

namespace wujihandcpp {

// USB identifiers for the tactile sensor board
static constexpr uint16_t TACTILE_USB_VID = 0x0483;
static constexpr uint16_t TACTILE_USB_PID = 0x5700;

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct TactileBoard::Impl {
    std::string serial_filter;   // empty = auto-discover first device
    std::string tty_path;        // resolved /dev/ttyACMx
    int fd = -1;                 // serial file descriptor

    std::atomic<bool> connected{false};
    std::atomic<TactileHandedness> last_hand{TactileHandedness::LEFT};

    // Streaming state
    std::thread reader_thread;
    std::atomic<bool> streaming{false};
    std::atomic<bool> stop_requested{false};

    // Ring buffer for frame synchronization.
    // We read into this buffer and scan for headers.
    uint8_t frame_buf[tactile_protocol::FRAME_SIZE]{};

    // -----------------------------------------------------------------------
    // Device discovery & open
    // -----------------------------------------------------------------------

    bool open_device() {
        auto devices = cdc::discover_devices(TACTILE_USB_VID, TACTILE_USB_PID);
        if (devices.empty()) return false;

        // Match serial number if specified
        const cdc::DeviceInfo* chosen = nullptr;
        if (!serial_filter.empty()) {
            for (const auto& d : devices) {
                if (d.serial_number == serial_filter) {
                    chosen = &d;
                    break;
                }
            }
            if (!chosen) return false;
        } else {
            chosen = &devices[0];
        }

        tty_path = chosen->tty_path;
        fd = cdc::open_cdc(tty_path.c_str());
        if (fd < 0) return false;

        connected.store(true, std::memory_order_release);
        return true;
    }

    void close_device() {
        connected.store(false, std::memory_order_release);
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }

    // -----------------------------------------------------------------------
    // Frame synchronization
    // -----------------------------------------------------------------------

    /// Read a single byte.
    /// Throws ConnectionLostError on disconnect, returns false on timeout.
    bool read_byte(uint8_t& byte, uint32_t timeout_ms) {
        ssize_t n = cdc::read_exact(fd, &byte, 1, timeout_ms);
        if (n < 0) throw ConnectionLostError("USB CDC device disconnected");
        return n == 1;
    }

    /// Read exactly `count` bytes into `buf`.
    /// Throws ConnectionLostError on disconnect, returns false on timeout.
    bool read_bytes(uint8_t* buf, size_t count, uint32_t timeout_ms) {
        ssize_t n = cdc::read_exact(fd, buf, count, timeout_ms);
        if (n < 0) throw ConnectionLostError("USB CDC device disconnected");
        return static_cast<size_t>(n) == count;
    }

    /// Synchronize and read one complete frame.
    /// Implements the scan-verify-read-CRC algorithm from the protocol spec.
    ///
    /// Returns true if a valid frame was placed in frame_buf.
    /// Throws ConnectionLostError on USB disconnect.
    /// Returns false on timeout.
    bool sync_and_read_frame(uint32_t timeout_ms) {
        // Scan for 0xAA 0x55 header
        uint8_t prev = 0;
        for (;;) {
            uint8_t b;
            if (!read_byte(b, timeout_ms)) {
                return false;  // timeout (disconnect already throws)
            }

            if (prev == tactile_protocol::HEADER_0 && b == tactile_protocol::HEADER_1) {
                // Found header. Place it in frame_buf.
                frame_buf[0] = tactile_protocol::HEADER_0;
                frame_buf[1] = tactile_protocol::HEADER_1;
                break;
            }
            prev = b;
        }

        // Read the remaining 1548 bytes (total 1550 - 2 header bytes already read)
        constexpr size_t remaining = tactile_protocol::FRAME_SIZE - 2;
        if (!read_bytes(frame_buf + 2, remaining, timeout_ms)) {
            return false;  // timeout (ConnectionLostError already thrown if disconnect)
        }

        // Verify length field == 1550
        uint16_t length = static_cast<uint16_t>(
            frame_buf[tactile_protocol::OFFSET_LENGTH] |
            (frame_buf[tactile_protocol::OFFSET_LENGTH + 1] << 8));
        if (length != tactile_protocol::EXPECTED_LENGTH) {
            // Bad length: this was a false header (0xAA55 in pressure data).
            // The caller should retry. We return false to signal "no valid frame".
            return false;
        }

        // CRC validation: firmware computes over bytes [2, 1548), skipping header
        uint16_t expected_crc = static_cast<uint16_t>(
            frame_buf[tactile_protocol::OFFSET_CRC] |
            (frame_buf[tactile_protocol::OFFSET_CRC + 1] << 8));
        uint16_t computed_crc = tactile_protocol::crc16_ccitt(
            frame_buf + tactile_protocol::OFFSET_LENGTH,
            tactile_protocol::OFFSET_CRC - tactile_protocol::OFFSET_LENGTH);

        if (expected_crc != computed_crc) {
            // CRC mismatch: could be a false header or corrupted frame.
            return false;
        }

        return true;
    }

    /// Read one valid frame with retries for false headers and CRC failures.
    TactileFrame read_one_frame(uint32_t timeout_ms) {
        // Allow up to ~20 sync attempts before giving up (handles false headers).
        // At 120 FPS, one frame takes ~8.3ms. With 100ms timeout, we can
        // attempt ~12 frames, which is generous for false-header recovery.
        constexpr int MAX_SYNC_ATTEMPTS = 20;

        for (int attempt = 0; attempt < MAX_SYNC_ATTEMPTS; ++attempt) {
            if (sync_and_read_frame(timeout_ms)) {
                TactileFrame frame = tactile_protocol::parse_frame(frame_buf);
                last_hand.store(frame.hand, std::memory_order_relaxed);
                return frame;
            }
            // sync_and_read_frame throws ConnectionLostError on disconnect,
            // so if we're here it's a false header, bad length, or CRC failure.
            // Continue scanning.
        }

        throw std::runtime_error("TactileBoard: timeout, no valid frame after "
                                 + std::to_string(MAX_SYNC_ATTEMPTS) + " sync attempts");
    }

    // -----------------------------------------------------------------------
    // Streaming thread
    // -----------------------------------------------------------------------

    void reader_loop(FrameCallback callback) {
        while (!stop_requested.load(std::memory_order_acquire)) {
            TactileFrame frame;
            try {
                frame = read_one_frame(100);
            } catch (const ConnectionLostError&) {
                // Notify caller with a zero-initialized frame
                TactileFrame empty{};
                try {
                    callback(empty);
                } catch (...) {
                    // Callback exception during disconnect notification: ignore
                }
                break;
            } catch (const std::runtime_error&) {
                // Timeout or sync failure: retry
                continue;
            }
            // Callback exceptions must not be swallowed as timeouts
            try {
                callback(frame);
            } catch (...) {
                break;
            }
        }
        streaming.store(false, std::memory_order_release);
    }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

TactileBoard::TactileBoard(const char* serial_number)
    : impl_(std::make_unique<Impl>()) {
    if (serial_number) impl_->serial_filter = serial_number;
}

TactileBoard::~TactileBoard() {
    disconnect();
}

bool TactileBoard::connect() {
    if (impl_->connected.load(std::memory_order_acquire)) return true;
    return impl_->open_device();
}

void TactileBoard::disconnect() {
    stop_streaming();
    impl_->close_device();
}

bool TactileBoard::is_connected() const {
    return impl_->connected.load(std::memory_order_acquire);
}

TactileFrame TactileBoard::read_frame(uint32_t timeout_ms) {
    if (!impl_->connected.load(std::memory_order_acquire))
        throw std::runtime_error("TactileBoard: not connected");
    if (impl_->streaming.load(std::memory_order_acquire))
        throw std::logic_error("TactileBoard: cannot call read_frame() while streaming");
    return impl_->read_one_frame(timeout_ms);
}

void TactileBoard::start_streaming(FrameCallback callback) {
    if (!impl_->connected.load(std::memory_order_acquire))
        throw std::runtime_error("TactileBoard: not connected");
    if (impl_->streaming.load(std::memory_order_acquire))
        throw std::logic_error("TactileBoard: already streaming");

    impl_->stop_requested.store(false, std::memory_order_release);
    impl_->streaming.store(true, std::memory_order_release);
    impl_->reader_thread = std::thread(&Impl::reader_loop, impl_.get(), std::move(callback));
}

void TactileBoard::stop_streaming() {
    impl_->stop_requested.store(true, std::memory_order_release);
    if (impl_->reader_thread.joinable()) {
        impl_->reader_thread.join();
    }
    impl_->streaming.store(false, std::memory_order_release);
}

TactileHandedness TactileBoard::handedness() const {
    return impl_->last_hand.load(std::memory_order_relaxed);
}

}  // namespace wujihandcpp
