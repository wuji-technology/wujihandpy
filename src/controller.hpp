#pragma once

#include <cstddef>

#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include <pybind11/numpy.h>
#include <wujihandcpp/device/hand.hpp>

namespace py = pybind11;

class IControllerWrapper final {
public:
    explicit IControllerWrapper(std::unique_ptr<wujihandcpp::device::IController> controller)
        : controller_(std::move(controller)) {}

    IControllerWrapper(const IControllerWrapper&) = delete;
    IControllerWrapper& operator=(const IControllerWrapper&) = delete;
    IControllerWrapper(IControllerWrapper&&) noexcept = default;
    IControllerWrapper& operator=(IControllerWrapper&&) noexcept = default;

    ~IControllerWrapper() = default;

    py::array_t<double> get_joint_actual_position() {
        if (!controller_)
            throw std::runtime_error("Controller is closed.");

        const auto& positions = controller_->get_joint_actual_position();

        auto buffer = new double[5 * 4];
        for (size_t i = 0; i < 5; i++)
            for (size_t j = 0; j < 4; j++)
                buffer[4 * i + j] = positions[i][j].load(std::memory_order::relaxed);
        py::capsule free(buffer, [](void* ptr) { delete[] static_cast<double*>(ptr); });

        return py::array_t<double>({5, 4}, buffer, free);
    }

    py::array_t<double> get_joint_actual_effort() {
        if (!controller_)
            throw std::runtime_error("Controller is closed.");

        const auto& efforts = controller_->get_joint_actual_effort();

        auto buffer = new double[5 * 4];
        for (size_t i = 0; i < 5; i++)
            for (size_t j = 0; j < 4; j++)
                buffer[4 * i + j] = efforts[i][j].load(std::memory_order::relaxed);
        py::capsule free(buffer, [](void* ptr) { delete[] static_cast<double*>(ptr); });

        return py::array_t<double>({5, 4}, buffer, free);
    }

    void set_joint_target_position(const py::array_t<double>& array) {
        if (!controller_)
            throw std::runtime_error("Controller is closed.");

        if (array.ndim() != 2 || array.shape()[0] != 5 || array.shape()[1] != 4)
            throw std::runtime_error("Array shape must be {5, 4}!");

        auto r = array.unchecked<2>();
        double target_positions[5][4];
        for (size_t i = 0; i < 5; ++i)
            for (size_t j = 0; j < 4; ++j)
                target_positions[i][j] = r(i, j);

        controller_->set_joint_target_position(target_positions);
    }

    void close() { controller_.reset(); }

private:
    std::unique_ptr<wujihandcpp::device::IController> controller_;
};
