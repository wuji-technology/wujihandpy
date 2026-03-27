#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>

#include "wujihandcpp/device/touch_board.hpp"

#include "logging/logging.hpp"
#include "protocol/tactile_parser.hpp"
#include "transport/transport.hpp"

namespace wujihandcpp::device {

struct TouchBoard::Impl {
    explicit Impl(const char* serial_number, uint16_t usb_pid, uint16_t usb_vid)
        : logger_(logging::get_logger())
        , transport_(transport::create_usb_transport(usb_vid, usb_pid, serial_number,
                                                     1,     // interface 1 (CDC data)
                                                     0x82,  // bulk IN endpoint
                                                     0x01)) // bulk OUT endpoint
    {

        transport_->receive([this](const std::byte* data, size_t size) {
            on_receive(data, size);
        });
    }

    ~Impl() { transport_.reset(); }

    void on_receive(const std::byte* data, size_t size) {
        // feed() processes all bytes and returns how many frames were parsed.
        // frame() returns the last one (fine for real-time latest-value semantics).
        int n = parser_.feed(data, size);
        if (n > 0) {
            const auto& f = parser_.frame();
            auto now = std::chrono::steady_clock::now();

            {
                std::lock_guard lock{mutex_};
                std::memcpy(raw_data_, f.data, sizeof(raw_data_));
                for (int r = 0; r < ROWS; ++r)
                    for (int c = 0; c < COLS; ++c)
                        normalized_data_[r][c] = std::clamp(
                            1.0f - raw_data_[r][c] / ADC_OPEN_CIRCUIT, 0.0f, 1.0f);
                handedness_.store(f.handedness, std::memory_order_relaxed);
                sequence_ = f.sequence;
                timestamp_ms_ = f.timestamp_ms;
                has_frame_.store(true, std::memory_order_release);
                // Count all frames parsed, not just 1
                frame_count_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);

                // FPS: add n entries and prune old ones
                for (int i = 0; i < n; ++i)
                    frame_times_.push_back(now);
                auto cutoff = now - std::chrono::seconds(1);
                while (!frame_times_.empty() && frame_times_.front() < cutoff)
                    frame_times_.pop_front();
            }

            cv_.notify_all();
        }
    }

    bool get_tactile(float (&out)[ROWS][COLS]) const {
        std::lock_guard lock{mutex_};
        if (!has_frame_.load(std::memory_order_acquire))
            return false;
        std::memcpy(out, normalized_data_, sizeof(out));
        return true;
    }

    bool get_tactile_raw(int16_t (&out)[ROWS][COLS]) const {
        std::lock_guard lock{mutex_};
        if (!has_frame_.load(std::memory_order_acquire))
            return false;
        std::memcpy(out, raw_data_, sizeof(raw_data_));
        return true;
    }

    /// Wait for a new frame under lock. Returns true if a new frame arrived before deadline.
    bool wait_for_new_frame(std::unique_lock<std::mutex>& lock, double timeout_seconds) {
        uint64_t before = frame_count_.load(std::memory_order_relaxed);
        auto deadline =
            std::chrono::steady_clock::now()
            + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::duration<double>(timeout_seconds));
        return cv_.wait_until(lock, deadline, [&]() {
            return frame_count_.load(std::memory_order_relaxed) > before;
        });
    }

    bool read_tactile(float (&out)[ROWS][COLS], double timeout_seconds) {
        std::unique_lock lock{mutex_};
        if (!wait_for_new_frame(lock, timeout_seconds))
            return false;
        std::memcpy(out, normalized_data_, sizeof(out));
        return true;
    }

    bool read_tactile_raw(int16_t (&out)[ROWS][COLS], double timeout_seconds) {
        std::unique_lock lock{mutex_};
        if (!wait_for_new_frame(lock, timeout_seconds))
            return false;
        std::memcpy(out, raw_data_, sizeof(out));
        return true;
    }

    int get_handedness() const {
        if (!has_frame_.load(std::memory_order_acquire))
            return -1;
        return handedness_.load(std::memory_order_relaxed);
    }

    float get_fps() const {
        std::lock_guard lock{mutex_};
        auto now = std::chrono::steady_clock::now();
        auto cutoff = now - std::chrono::seconds(1);
        while (!frame_times_.empty() && frame_times_.front() < cutoff)
            frame_times_.pop_front();
        return static_cast<float>(frame_times_.size());
    }

    uint64_t get_frame_count() const {
        return frame_count_.load(std::memory_order_relaxed);
    }

    logging::Logger& logger_;
    std::unique_ptr<transport::ITransport> transport_;
    protocol::TactileParser parser_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    int16_t raw_data_[ROWS][COLS]{};
    float normalized_data_[ROWS][COLS]{};
    std::atomic<uint8_t> handedness_{0xFF};
    uint16_t sequence_{0};
    uint32_t timestamp_ms_{0};
    std::atomic<bool> has_frame_{false};
    std::atomic<uint64_t> frame_count_{0};

    // frame timestamps for FPS calculation (within last 1 second)
    mutable std::deque<std::chrono::steady_clock::time_point> frame_times_;
};

TouchBoard::TouchBoard(const char* serial_number, uint16_t usb_pid, uint16_t usb_vid)
    : impl_(std::make_unique<Impl>(serial_number, usb_pid, usb_vid)) {}

TouchBoard::~TouchBoard() = default;

bool TouchBoard::get_tactile(float (&out)[ROWS][COLS]) const {
    return impl_->get_tactile(out);
}

bool TouchBoard::get_tactile_raw(int16_t (&out)[ROWS][COLS]) const {
    return impl_->get_tactile_raw(out);
}

bool TouchBoard::read_tactile(float (&out)[ROWS][COLS], double timeout_seconds) {
    return impl_->read_tactile(out, timeout_seconds);
}

bool TouchBoard::read_tactile_raw(int16_t (&out)[ROWS][COLS], double timeout_seconds) {
    return impl_->read_tactile_raw(out, timeout_seconds);
}

int TouchBoard::get_handedness() const {
    return impl_->get_handedness();
}

float TouchBoard::get_fps() const {
    return impl_->get_fps();
}

uint64_t TouchBoard::get_frame_count() const {
    return impl_->get_frame_count();
}

} // namespace wujihandcpp::device
