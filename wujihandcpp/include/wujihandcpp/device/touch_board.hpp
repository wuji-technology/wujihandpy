#pragma once

#include <cstdint>

#include <memory>

#include "wujihandcpp/utility/api.hpp"

namespace wujihandcpp::device {

class WUJIHANDCPP_API TouchBoard {
public:
    static constexpr int ROWS = 24;
    static constexpr int COLS = 32;
    static constexpr uint16_t DEFAULT_USB_PID = 0x5700;
    static constexpr uint16_t DEFAULT_USB_VID = 0x0483;
    static constexpr float ADC_OPEN_CIRCUIT = 2135.0f;

    explicit TouchBoard(
        const char* serial_number = nullptr, uint16_t usb_pid = DEFAULT_USB_PID,
        uint16_t usb_vid = DEFAULT_USB_VID);

    ~TouchBoard();

    TouchBoard(const TouchBoard&) = delete;
    TouchBoard& operator=(const TouchBoard&) = delete;
    TouchBoard(TouchBoard&&) = delete;
    TouchBoard& operator=(TouchBoard&&) = delete;

    /// Get latest tactile data (normalized 0.0~1.0), non-blocking.
    /// Returns false if no frame has been received yet.
    bool get_tactile(float (&out)[ROWS][COLS]) const;

    /// Get latest raw tactile data (i16), non-blocking.
    /// Returns false if no frame has been received yet.
    bool get_tactile_raw(int16_t (&out)[ROWS][COLS]) const;

    /// Block until next frame arrives, then return normalized data.
    /// Returns false on timeout.
    bool read_tactile(float (&out)[ROWS][COLS], double timeout_seconds = 1.0);

    /// Block until next frame arrives, then return raw data.
    /// Returns false on timeout.
    bool read_tactile_raw(int16_t (&out)[ROWS][COLS], double timeout_seconds = 1.0);

    /// Get handedness: 0=left, 1=right, -1=unknown.
    int get_handedness() const;

    /// Get current receive FPS (frames per second).
    float get_fps() const;

    /// Get total frame count received so far.
    uint64_t get_frame_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wujihandcpp::device
