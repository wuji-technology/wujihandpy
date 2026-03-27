#pragma once

#include <cstdint>
#include <cstring>

#include <optional>
#include <string>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <wujihandcpp/device/touch_board.hpp>

namespace py = pybind11;

class TouchBoardWrapper {
public:
    static constexpr int ROWS = wujihandcpp::device::TouchBoard::ROWS;
    static constexpr int COLS = wujihandcpp::device::TouchBoard::COLS;

    explicit TouchBoardWrapper(
        std::optional<std::string> serial_number, uint16_t usb_pid, uint16_t usb_vid)
        : board_(serial_number ? serial_number->c_str() : nullptr, usb_pid, usb_vid) {}

    /// Blocking read, returns normalized float32 (24,32). Throws on timeout.
    bool read_tactile_impl(float (&out)[ROWS][COLS], double timeout) {
        return board_.read_tactile(out, timeout);
    }

    /// Blocking read, returns raw int16 (24,32). Throws on timeout.
    bool read_tactile_raw_impl(int16_t (&out)[ROWS][COLS], double timeout) {
        return board_.read_tactile_raw(out, timeout);
    }

    /// Non-blocking get normalized data, returns numpy or None.
    py::object get_tactile() {
        float buf[ROWS][COLS];
        if (!board_.get_tactile(buf))
            return py::none();
        return make_float_array(buf);
    }

    /// Non-blocking get raw data, returns numpy or None.
    py::object get_tactile_raw() {
        int16_t buf[ROWS][COLS];
        if (!board_.get_tactile_raw(buf))
            return py::none();
        return make_int16_array(buf);
    }

    std::string get_handedness() const {
        int h = board_.get_handedness();
        if (h == 0)
            return "left";
        if (h == 1)
            return "right";
        return "unknown";
    }

    float get_fps() const { return board_.get_fps(); }

    uint64_t get_frame_count() const { return board_.get_frame_count(); }

    static py::array_t<float> make_float_array(const float (&buf)[ROWS][COLS]) {
        auto* data = new float[ROWS * COLS];
        std::memcpy(data, buf, sizeof(float) * ROWS * COLS);
        py::capsule free(data, [](void* p) { delete[] static_cast<float*>(p); });
        return py::array_t<float>({ROWS, COLS}, data, free);
    }

    static py::array_t<int16_t> make_int16_array(const int16_t (&buf)[ROWS][COLS]) {
        auto* data = new int16_t[ROWS * COLS];
        std::memcpy(data, buf, sizeof(int16_t) * ROWS * COLS);
        py::capsule free(data, [](void* p) { delete[] static_cast<int16_t*>(p); });
        return py::array_t<int16_t>({ROWS, COLS}, data, free);
    }

private:
    wujihandcpp::device::TouchBoard board_;
};
