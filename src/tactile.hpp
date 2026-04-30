#pragma once

#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstring>
#include <memory>

#include <wujihandcpp/data/tactile.hpp>
#include <wujihandcpp/data/tactile_device.hpp>
#include <wujihandcpp/device/tactile_board.hpp>
#include <wujihandcpp/protocol/tactile_command.hpp>

namespace py = pybind11;

namespace tactile {

inline void init_module(py::module_& m) {
    using namespace wujihandcpp;

    // --- Enums ---
    py::enum_<TactileHandedness>(m, "TactileHandedness")
        .value("LEFT", TactileHandedness::LEFT)
        .value("RIGHT", TactileHandedness::RIGHT);

    py::enum_<TactileStatus>(m, "TactileStatus")
        .value("OK", TactileStatus::Ok)
        .value("BAD_LENGTH", TactileStatus::BadLength)
        .value("BAD_CRC", TactileStatus::BadCrc)
        .value("UNKNOWN_CMD", TactileStatus::UnknownCmd)
        .value("BAD_PAYLOAD", TactileStatus::BadPayload);

    // --- Exception (status is encoded in the message string for now) ---
    py::register_exception<TactileError>(m, "TactileError");

    // --- TactileFrame ---
    py::class_<TactileFrame>(m, "TactileFrame")
        .def_readonly("hand", &TactileFrame::hand)
        .def_readonly("sequence", &TactileFrame::sequence)
        .def_readonly("timestamp_ms", &TactileFrame::timestamp_ms)
        .def_readonly("crc_valid", &TactileFrame::crc_valid)
        .def_property_readonly("pressure", [](const TactileFrame& f) {
            // Return a 24x32 float32 numpy array. memcpy preserves NaN bit
            // patterns; callers detect invalid cells via numpy.isnan().
            auto* buf = new float[24 * 32];
            std::memcpy(buf, &f.pressure[0][0], 24 * 32 * sizeof(float));
            py::capsule free(buf, [](void* p) { delete[] static_cast<float*>(p); });
            return py::array_t<float>({24, 32}, buf, free);
        });

    // --- POD reply types ---
    py::class_<TactileDeviceInfo>(m, "TactileDeviceInfo")
        .def_readonly("serial", &TactileDeviceInfo::serial)
        .def_readonly("hw_revision", &TactileDeviceInfo::hw_revision)
        .def_readonly("fw_version", &TactileDeviceInfo::fw_version);

    py::class_<TactileFwBuild>(m, "TactileFwBuild")
        .def_readonly("git_short_sha", &TactileFwBuild::git_short_sha);

    py::class_<TactileDiagnostics>(m, "TactileDiagnostics")
        .def_readonly("uptime_ms", &TactileDiagnostics::uptime_ms)
        .def_readonly("frame_count", &TactileDiagnostics::frame_count)
        .def_readonly("crc_err_count", &TactileDiagnostics::crc_err_count)
        .def_readonly("dropout_count", &TactileDiagnostics::dropout_count)
        .def_readonly("usb_reset_count", &TactileDiagnostics::usb_reset_count);

    py::class_<TactileDeviceTime>(m, "TactileDeviceTime")
        .def_readonly("device_monotonic_ns", &TactileDeviceTime::device_monotonic_ns);

    py::class_<TactileSyncResult>(m, "TactileSyncResult")
        .def_readonly("device_ns_at_sync", &TactileSyncResult::device_ns_at_sync)
        .def_readonly("host_ns_echo", &TactileSyncResult::host_ns_echo);

    // --- TactileBoard device ---
    //
    // Held by shared_ptr with a custom GIL-releasing deleter so that when
    // Python GC destroys the wrapper while a streaming thread is exiting,
    // the destructor does not block on a thread that needs the GIL to
    // destroy its captured Python callback (see deleter pattern below).
    py::class_<TactileBoard, std::shared_ptr<TactileBoard>>(m, "TactileBoard")
        .def(py::init([](std::optional<std::string> serial_number) {
            const char* sn = serial_number.has_value()
                ? serial_number.value().c_str() : nullptr;
            return std::shared_ptr<TactileBoard>(
                new TactileBoard(sn),
                [](TactileBoard* p) {
                    // Release the GIL before running ~TactileBoard, which
                    // calls disconnect() and joins the streaming /
                    // demuxer-reader threads. Either of those threads may
                    // need to acquire the GIL to destroy a captured
                    // py::function — without this release we deadlock.
                    py::gil_scoped_release release;
                    delete p;
                });
        }), py::arg("serial_number") = py::none())

        // Connection
        .def("connect", [](TactileBoard& self) {
            py::gil_scoped_release release;
            return self.connect();
        })
        .def("disconnect", [](TactileBoard& self) {
            py::gil_scoped_release release;
            self.disconnect();
        })
        .def("is_connected", &TactileBoard::is_connected)

        // Frame reading
        .def("read_frame", [](TactileBoard& self, uint32_t timeout_ms) {
            py::gil_scoped_release release;
            return self.read_frame(timeout_ms);
        }, py::arg("timeout_ms") = 100)

        // Streaming
        .def("start_streaming", [](TactileBoard& self, py::function callback) {
            // Wrap user callback in a shared_ptr that releases under GIL on dtor.
            auto callback_ptr = std::shared_ptr<py::function>(
                new py::function(std::move(callback)),
                [](py::function* cb) {
                    py::gil_scoped_acquire acquire;
                    delete cb;
                });
            auto cpp_cb = [cb = std::move(callback_ptr)](const TactileFrame& frame) {
                py::gil_scoped_acquire acquire;
                try {
                    (*cb)(frame);
                } catch (py::error_already_set& e) {
                    e.restore();
                    PyErr_Clear();
                    throw std::runtime_error(
                        "TactileBoard: Python frame callback raised an exception");
                }
            };
            // Release the GIL across self.start_streaming(...) — internally
            // it joins any previously-exited streaming thread, and that
            // thread's destruction needs the GIL to drop its captured
            // py::function. Holding the GIL here would deadlock.
            py::gil_scoped_release release;
            self.start_streaming(std::move(cpp_cb));
        }, py::arg("callback"))

        .def("stop_streaming", [](TactileBoard& self) {
            py::gil_scoped_release release;
            self.stop_streaming();
        })

        .def("set_disconnect_callback", [](TactileBoard& self, py::function callback) {
            auto callback_ptr = std::shared_ptr<py::function>(
                new py::function(std::move(callback)),
                [](py::function* cb) {
                    py::gil_scoped_acquire acquire;
                    delete cb;
                });
            auto cpp_cb = [cb = std::move(callback_ptr)]() {
                py::gil_scoped_acquire acquire;
                try {
                    (*cb)();
                } catch (py::error_already_set& e) {
                    e.restore();
                    PyErr_Clear();
                    // Disconnect callback exceptions are intentionally swallowed
                    // (see C++ TactileBoard::set_disconnect_callback contract).
                }
            };
            // Release the GIL — set_disconnect_callback may drop the
            // previous callback's shared_ptr, whose deleter needs the GIL.
            py::gil_scoped_release release;
            self.set_disconnect_callback(std::move(cpp_cb));
        }, py::arg("callback"))

        // Identity (spec §3.1)
        .def("get_device_info", [](TactileBoard& self) {
            py::gil_scoped_release release;
            return self.get_device_info();
        })
        .def("get_fw_build", [](TactileBoard& self) {
            py::gil_scoped_release release;
            return self.get_fw_build();
        })
        .def("get_handedness", [](TactileBoard& self) {
            py::gil_scoped_release release;
            return self.get_handedness();
        })

        // Diagnostics (spec §3.2)
        .def("get_diagnostics", [](TactileBoard& self) {
            py::gil_scoped_release release;
            return self.get_diagnostics();
        })
        .def("reset_counters", [](TactileBoard& self) {
            py::gil_scoped_release release;
            self.reset_counters();
        })

        // Lifecycle (spec §3.3)
        .def("set_streaming", [](TactileBoard& self, bool enable) {
            py::gil_scoped_release release;
            self.set_streaming(enable);
        }, py::arg("enable"))
        .def("reset_device", [](TactileBoard& self) {
            py::gil_scoped_release release;
            self.reset_device();
        })
        .def("enter_bootloader", [](TactileBoard& self, uint32_t magic) {
            py::gil_scoped_release release;
            self.enter_bootloader(magic);
        }, py::arg("magic"))

        // Configuration (spec §3.4)
        .def("get_sample_rate_hz", [](TactileBoard& self) {
            py::gil_scoped_release release;
            return self.get_sample_rate_hz();
        })
        .def("set_sample_rate_hz", [](TactileBoard& self, uint16_t hz) {
            py::gil_scoped_release release;
            self.set_sample_rate_hz(hz);
        }, py::arg("sample_rate_hz"))
        .def("get_streaming_enabled", [](TactileBoard& self) {
            py::gil_scoped_release release;
            return self.get_streaming_enabled();
        })

        // Time sync (spec §3.5)
        .def("get_device_time", [](TactileBoard& self) {
            py::gil_scoped_release release;
            return self.get_device_time();
        })
        .def("sync_host_epoch", [](TactileBoard& self, uint64_t host_unix_ns) {
            py::gil_scoped_release release;
            return self.sync_host_epoch(host_unix_ns);
        }, py::arg("host_unix_ns"))

        // Context manager
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
        })

        // Module-level constants
        .def_property_readonly_static("BOOTLOADER_MAGIC",
            [](py::object) { return TACTILE_BOOTLOADER_MAGIC; });
}

}  // namespace tactile
