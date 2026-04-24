#pragma once

#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <memory>

#include <wujihandcpp/data/tactile.hpp>
#include <wujihandcpp/device/tactile_board.hpp>

namespace py = pybind11;

namespace tactile {

inline void init_module(py::module_& m) {
    using namespace wujihandcpp;

    // --- Enum ---
    py::enum_<TactileHandedness>(m, "TactileHandedness")
        .value("LEFT", TactileHandedness::LEFT)
        .value("RIGHT", TactileHandedness::RIGHT);

    // --- TactileFrame data class ---
    py::class_<TactileFrame>(m, "TactileFrame")
        .def_readonly("hand", &TactileFrame::hand)
        .def_readonly("sequence", &TactileFrame::sequence)
        .def_readonly("timestamp_ms", &TactileFrame::timestamp_ms)
        .def_readonly("crc_valid", &TactileFrame::crc_valid)
        .def_property_readonly("pressure", [](const TactileFrame& f) {
            // Return a copy as numpy array (24x32, ~1.5KB, <1us)
            auto* buf = new int16_t[24 * 32];
            for (int r = 0; r < 24; ++r)
                for (int c = 0; c < 32; ++c)
                    buf[r * 32 + c] = f.pressure[r][c];

            py::capsule free(buf, [](void* ptr) {
                delete[] static_cast<int16_t*>(ptr);
            });
            return py::array_t<int16_t>({24, 32}, buf, free);
        });

    // --- TactileBoard device ---
    py::class_<TactileBoard>(m, "TactileBoard")
        .def(py::init([](std::optional<std::string> serial_number) {
            const char* sn = serial_number.has_value()
                ? serial_number.value().c_str() : nullptr;
            return std::make_unique<TactileBoard>(sn);
        }), py::arg("serial_number") = py::none())

        .def("connect", [](TactileBoard& self) {
            py::gil_scoped_release release;
            return self.connect();
        })

        .def("disconnect", [](TactileBoard& self) {
            py::gil_scoped_release release;
            self.disconnect();
        })

        .def("is_connected", &TactileBoard::is_connected)

        .def("read_frame", [](TactileBoard& self, uint32_t timeout_ms) {
            py::gil_scoped_release release;
            return self.read_frame(timeout_ms);
        }, py::arg("timeout_ms") = 100)

        .def("start_streaming", [](TactileBoard& self, py::function callback) {
            auto callback_ptr = std::shared_ptr<py::function>(
                new py::function(std::move(callback)),
                [](py::function* callback) {
                    py::gil_scoped_acquire acquire;
                    delete callback;
                });

            // The C++ callback runs on the reader thread without the GIL.
            // We must acquire the GIL before calling back into Python.
            self.start_streaming([callback = std::move(callback_ptr)](
                    const TactileFrame& frame) {
                py::gil_scoped_acquire acquire;
                try {
                    (*callback)(frame);
                } catch (py::error_already_set& e) {
                    e.restore();
                    PyErr_Clear();
                    throw std::runtime_error(
                        "TactileBoard: Python callback raised an exception");
                }
            });
        }, py::arg("callback"))

        .def("stop_streaming", [](TactileBoard& self) {
            py::gil_scoped_release release;
            self.stop_streaming();
        })

        .def("handedness", &TactileBoard::handedness)

        // Context manager support
        .def("__enter__", [](TactileBoard& self) -> TactileBoard& {
            bool ok;
            { py::gil_scoped_release release; ok = self.connect(); }
            if (!ok) throw std::runtime_error("TactileBoard: device not found");
            return self;
        })
        .def("__exit__", [](TactileBoard& self, const py::object&,
                            const py::object&, const py::object&) {
            py::gil_scoped_release release;
            self.disconnect();
        });
}

}  // namespace tactile
