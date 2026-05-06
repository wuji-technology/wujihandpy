#pragma once

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

namespace tactile_binding {

// Register pybind classes into the `wujihandpy._core.tactile` submodule.
// The `wujihandpy.tactile` user-facing import path is provided by a small
// Python wrapper at `src/wujihandpy/tactile.py` that re-exports from here.
inline void init_module(py::module_& parent) {
    using namespace wujihandcpp::tactile;

    py::module_ m = parent.def_submodule(
        "tactile",
        "Tactile board API (wujihandcpp::tactile namespace).");

    // --- Enums ---
    py::enum_<Handedness>(m, "Handedness")
        .value("LEFT", Handedness::LEFT)
        .value("RIGHT", Handedness::RIGHT);

    py::enum_<Status>(m, "Status")
        .value("OK", Status::Ok)
        .value("BAD_LENGTH", Status::BadLength)
        .value("BAD_CRC", Status::BadCrc)
        .value("UNKNOWN_CMD", Status::UnknownCmd)
        .value("BAD_PAYLOAD", Status::BadPayload);

    // --- Exception (status is encoded in the message string for now) ---
    py::register_exception<Error>(m, "Error");

    // --- Frame ---
    py::class_<Frame>(m, "Frame")
        .def_readonly("hand", &Frame::hand)
        .def_readonly("sequence", &Frame::sequence)
        .def_readonly("timestamp_ms", &Frame::timestamp_ms)
        .def_property_readonly("pressure", [](const Frame& f) {
            // Return a 24x32 float32 numpy array. memcpy preserves NaN bit
            // patterns; callers detect invalid cells via numpy.isnan().
            auto* buf = new float[24 * 32];
            std::memcpy(buf, &f.pressure[0][0], 24 * 32 * sizeof(float));
            py::capsule free(buf, [](void* p) { delete[] static_cast<float*>(p); });
            return py::array_t<float>({24, 32}, buf, free);
        });

    // --- POD reply types ---
    py::class_<DeviceInfo>(m, "DeviceInfo")
        .def_readonly("serial", &DeviceInfo::serial)
        .def_readonly("hw_revision", &DeviceInfo::hw_revision)
        .def_readonly("fw_version", &DeviceInfo::fw_version);

    py::class_<FwBuild>(m, "FwBuild")
        .def_readonly("git_short_sha", &FwBuild::git_short_sha);

    py::class_<Diagnostics>(m, "Diagnostics")
        .def_readonly("uptime_ms", &Diagnostics::uptime_ms)
        .def_readonly("frame_count", &Diagnostics::frame_count)
        .def_readonly("crc_err_count", &Diagnostics::crc_err_count)
        .def_readonly("dropout_count", &Diagnostics::dropout_count)
        .def_readonly("usb_reset_count", &Diagnostics::usb_reset_count);

    py::class_<DeviceTime>(m, "DeviceTime")
        .def_readonly("device_monotonic_ns", &DeviceTime::device_monotonic_ns);

    py::class_<SyncResult>(m, "SyncResult")
        .def_readonly("device_ns_at_sync", &SyncResult::device_ns_at_sync)
        .def_readonly("host_ns_echo", &SyncResult::host_ns_echo);

    // --- Board device ---
    //
    // Held by shared_ptr with a custom GIL-releasing deleter so that when
    // Python GC destroys the wrapper while a streaming thread is exiting,
    // the destructor does not block on a thread that needs the GIL to
    // destroy its captured Python callback (see deleter pattern below).
    py::class_<Board, std::shared_ptr<Board>>(m, "Board")
        .def(py::init([](std::optional<std::string> serial_number) {
            const char* sn = serial_number.has_value()
                ? serial_number.value().c_str() : nullptr;
            return std::shared_ptr<Board>(
                new Board(sn),
                [](Board* p) {
                    // Release the GIL before running ~Board, which calls
                    // disconnect() and joins the streaming / demuxer-reader
                    // threads. Either of those threads may need to acquire
                    // the GIL to destroy a captured py::function — without
                    // this release we deadlock.
                    py::gil_scoped_release release;
                    delete p;
                });
        }), py::arg("serial_number") = py::none())

        // Connection
        .def("connect", [](Board& self) {
            py::gil_scoped_release release;
            return self.connect();
        })
        .def("disconnect", [](Board& self) {
            py::gil_scoped_release release;
            self.disconnect();
        })
        .def("is_connected", &Board::is_connected)

        // Frame reading
        .def("read_frame", [](Board& self, uint32_t timeout_ms) {
            py::gil_scoped_release release;
            return self.read_frame(timeout_ms);
        }, py::arg("timeout_ms") = 100)

        // Streaming
        .def("start_streaming", [](Board& self, py::function callback) {
            // Wrap user callback in a shared_ptr that releases under GIL on dtor.
            auto callback_ptr = std::shared_ptr<py::function>(
                new py::function(std::move(callback)),
                [](py::function* cb) {
                    py::gil_scoped_acquire acquire;
                    delete cb;
                });
            auto cpp_cb = [cb = std::move(callback_ptr)](const Frame& frame) {
                py::gil_scoped_acquire acquire;
                try {
                    (*cb)(frame);
                } catch (py::error_already_set& e) {
                    // Surface the Python traceback via sys.unraisablehook
                    // so the user sees what went wrong before the
                    // streaming consumer tears down. pybind11's
                    // `discard_as_unraisable` is the correct one-call:
                    // it RESTORES the cached error to the Python error
                    // indicator and calls PyErr_WriteUnraisable internally.
                    // Calling PyErr_WriteUnraisable directly without the
                    // restore is a no-op — error_already_set's constructor
                    // already fetched + cleared the indicator, so there's
                    // no exception left to print.
                    e.discard_as_unraisable(*cb);
                    throw std::runtime_error(
                        "tactile.Board: Python frame callback raised an exception");
                }
            };
            // Release the GIL across self.start_streaming(...) — internally
            // it joins any previously-exited streaming thread, and that
            // thread's destruction needs the GIL to drop its captured
            // py::function. Holding the GIL here would deadlock.
            py::gil_scoped_release release;
            self.start_streaming(std::move(cpp_cb));
        }, py::arg("callback"))

        .def("stop_streaming", [](Board& self) {
            py::gil_scoped_release release;
            self.stop_streaming();
        })

        .def("set_disconnect_callback", [](Board& self, py::function callback) {
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
                    // Disconnect callback exceptions are intentionally
                    // swallowed (see C++ Board::set_disconnect_callback
                    // contract — the disconnect path can't unwind the
                    // SDK reader thread cleanly mid-shutdown). Use
                    // pybind11's `discard_as_unraisable` to restore the
                    // cached error AND print the traceback via
                    // sys.unraisablehook in one call. Calling
                    // PyErr_WriteUnraisable alone (or `e.restore();
                    // PyErr_Clear()`) was a silent no-op — both paths
                    // depended on the error indicator being live, but
                    // error_already_set's constructor already fetched
                    // and cleared it.
                    e.discard_as_unraisable(*cb);
                }
            };
            // Release the GIL — set_disconnect_callback may drop the
            // previous callback's shared_ptr, whose deleter needs the GIL.
            py::gil_scoped_release release;
            self.set_disconnect_callback(std::move(cpp_cb));
        }, py::arg("callback"))

        // Identity (spec §3.1)
        .def("get_device_info", [](Board& self) {
            py::gil_scoped_release release;
            return self.get_device_info();
        })
        .def("get_fw_build", [](Board& self) {
            py::gil_scoped_release release;
            return self.get_fw_build();
        })
        .def("get_handedness", [](Board& self) {
            py::gil_scoped_release release;
            return self.get_handedness();
        })

        // Diagnostics (spec §3.2)
        .def("get_diagnostics", [](Board& self) {
            py::gil_scoped_release release;
            return self.get_diagnostics();
        })
        .def("reset_counters", [](Board& self) {
            py::gil_scoped_release release;
            self.reset_counters();
        })

        // Lifecycle (spec §3.3)
        .def("set_streaming", [](Board& self, bool enable) {
            py::gil_scoped_release release;
            self.set_streaming(enable);
        }, py::arg("enable"))
        .def("reset_device", [](Board& self) {
            py::gil_scoped_release release;
            self.reset_device();
        })
        .def("enter_bootloader", [](Board& self, uint32_t magic) {
            py::gil_scoped_release release;
            self.enter_bootloader(magic);
        }, py::arg("magic"))

        // Configuration (spec §3.4)
        .def("get_sample_rate_hz", [](Board& self) {
            py::gil_scoped_release release;
            return self.get_sample_rate_hz();
        })
        .def("set_sample_rate_hz", [](Board& self, uint16_t hz) {
            py::gil_scoped_release release;
            self.set_sample_rate_hz(hz);
        }, py::arg("sample_rate_hz"))
        .def("get_streaming_enabled", [](Board& self) {
            py::gil_scoped_release release;
            return self.get_streaming_enabled();
        })

        // Time sync (spec §3.5)
        .def("get_device_time", [](Board& self) {
            py::gil_scoped_release release;
            return self.get_device_time();
        })
        .def("sync_host_epoch", [](Board& self, uint64_t host_unix_ns) {
            py::gil_scoped_release release;
            return self.sync_host_epoch(host_unix_ns);
        }, py::arg("host_unix_ns"))

        // Context manager
        .def("__enter__", [](Board& self) -> Board& {
            bool ok;
            { py::gil_scoped_release release; ok = self.connect(); }
            // NotConnectedError → Python ConnectionError (see main.cpp), so
            // `except ConnectionError` matches the rest of the tactile surface.
            if (!ok) throw NotConnectedError("tactile.Board: device not found");
            return self;
        })
        .def("__exit__", [](Board& self, const py::object&,
                            const py::object&, const py::object&) {
            py::gil_scoped_release release;
            self.disconnect();
        });

    // Module-level constants on the submodule, not the class.
    m.attr("BOOTLOADER_MAGIC") = BOOTLOADER_MAGIC;

    // Publish __all__ so `from wujihandpy._core.tactile import *` works and so
    // wrapper tooling (src/wujihandpy/tactile.py + update_stubs.py) reads a
    // single source of truth for the public submodule surface. Derive it
    // from the module's own dict — every public binding registered above
    // shows up automatically; adding a new py::class_ or m.attr never needs
    // a paired __all__ edit, eliminating the wrapper/stub-drift class
    // documented in the round-1 review.
    {
        py::list names;
        for (auto item : m.attr("__dict__").cast<py::dict>()) {
            std::string name = item.first.cast<std::string>();
            if (!name.empty() && name[0] != '_') {
                names.append(name);
            }
        }
        m.attr("__all__") = py::module_::import("builtins").attr("sorted")(names);
    }
}

}  // namespace tactile_binding
