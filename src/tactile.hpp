#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstring>
#include <memory>

#include <wujihandcpp/data/tactile.hpp>
#include <wujihandcpp/data/tactile_device.hpp>
#include <wujihandcpp/device/tactile_glove.hpp>
#include <wujihandcpp/protocol/tactile_command.hpp>

namespace py = pybind11;

namespace tactile_binding {

namespace {

// Wrap a Python callable as a C++ functor that acquires the GIL on each
// call. Held by shared_ptr with a GIL-aware deleter so the captured
// py::function can be destroyed even if the last ref drops on a non-Python
// thread (e.g. the SDK reader thread on shutdown).
template <typename... Args>
auto make_python_callback(py::function py_cb, const char* error_context) {
    auto cb = std::shared_ptr<py::function>(
        new py::function(std::move(py_cb)),
        [](py::function* p) {
            py::gil_scoped_acquire acquire;
            delete p;
        });
    return [cb = std::move(cb), error_context](Args... args) {
        py::gil_scoped_acquire acquire;
        try {
            (*cb)(std::forward<Args>(args)...);
        } catch (py::error_already_set& e) {
            // Preserve callback failures through sys.unraisablehook.
            e.discard_as_unraisable(*cb);
            if (error_context) throw std::runtime_error(error_context);
        }
    };
}

}  // namespace

inline void init_module(py::module_& parent) {
    using namespace wujihandcpp::tactile;
    using py::call_guard;
    using release_gil = py::gil_scoped_release;

    py::module_ m = parent.def_submodule(
        "tactile",
        "Tactile glove API (wujihandcpp::tactile namespace).");

    py::enum_<Handedness>(m, "Handedness")
        .value("LEFT", Handedness::LEFT)
        .value("RIGHT", Handedness::RIGHT);

    py::enum_<Status>(m, "Status")
        .value("OK", Status::Ok)
        .value("BAD_LENGTH", Status::BadLength)
        .value("BAD_CRC", Status::BadCrc)
        .value("UNKNOWN_CMD", Status::UnknownCmd)
        .value("BAD_PAYLOAD", Status::BadPayload);

    py::register_exception<Error>(m, "Error");

    py::class_<Frame>(m, "Frame")
        .def_readonly("hand", &Frame::hand)
        .def_readonly("sequence", &Frame::sequence)
        .def_readonly("timestamp_ms", &Frame::timestamp_ms)
        .def_property_readonly("pressure", [](const Frame& f) {
            // memcpy preserves NaN bit patterns; callers use numpy.isnan().
            py::array_t<float> out({24, 32});
            std::memcpy(out.mutable_data(), &f.pressure[0][0],
                        24 * 32 * sizeof(float));
            return out;
        });

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

    // Glove is held by shared_ptr with a GIL-releasing deleter: ~Glove joins
    // streaming / reader threads, which may need the GIL to destroy captured
    // Python callbacks. Holding the GIL through the join would deadlock.
    py::class_<Glove, std::shared_ptr<Glove>>(m, "Glove")
        .def(py::init([](std::optional<std::string> serial_number) {
            const char* sn = serial_number.has_value()
                ? serial_number.value().c_str() : nullptr;
            return std::shared_ptr<Glove>(
                new Glove(sn),
                [](Glove* p) {
                    py::gil_scoped_release release;
                    delete p;
                });
        }), py::arg("serial_number") = py::none())

        .def("connect",          &Glove::connect,          call_guard<release_gil>())
        .def("disconnect",       &Glove::disconnect,       call_guard<release_gil>())
        .def("is_connected",     &Glove::is_connected)
        .def("read_frame",       &Glove::read_frame,       py::arg("timeout_ms") = 100,
                                                           call_guard<release_gil>())
        .def("stop_streaming",   &Glove::stop_streaming,   call_guard<release_gil>())

        .def("start_streaming", [](Glove& self, py::function cb) {
            auto wrapped = make_python_callback<const Frame&>(
                std::move(cb),
                "TactileGlove: Python frame callback raised an exception");
            py::gil_scoped_release release;
            self.start_streaming(std::move(wrapped));
        }, py::arg("callback"))

        .def("set_disconnect_callback", [](Glove& self, py::function cb) {
            // Disconnect-callback exceptions are intentionally swallowed
            // (SDK contract — can't unwind the reader thread mid-shutdown);
            // pass nullptr to suppress the C++ rethrow.
            auto wrapped = make_python_callback<>(std::move(cb), nullptr);
            py::gil_scoped_release release;
            self.set_disconnect_callback(std::move(wrapped));
        }, py::arg("callback"))

        .def("get_device_info",       &Glove::get_device_info,       call_guard<release_gil>())
        .def("get_fw_build",          &Glove::get_fw_build,          call_guard<release_gil>())
        .def("get_handedness",        &Glove::get_handedness,        call_guard<release_gil>())
        .def("get_diagnostics",       &Glove::get_diagnostics,       call_guard<release_gil>())
        .def("reset_counters",        &Glove::reset_counters,        call_guard<release_gil>())
        .def("set_streaming",         &Glove::set_streaming,         py::arg("enable"),
                                                                     call_guard<release_gil>())
        .def("reset_device",          &Glove::reset_device,          call_guard<release_gil>())
        .def("enter_bootloader",      &Glove::enter_bootloader,      py::arg("magic"),
                                                                     call_guard<release_gil>())
        .def("get_sample_rate_hz",    &Glove::get_sample_rate_hz,    call_guard<release_gil>())
        .def("set_sample_rate_hz",    &Glove::set_sample_rate_hz,    py::arg("sample_rate_hz"),
                                                                     call_guard<release_gil>())
        .def("get_streaming_enabled", &Glove::get_streaming_enabled, call_guard<release_gil>())
        .def("get_device_time",       &Glove::get_device_time,       call_guard<release_gil>())
        .def("sync_host_epoch",       &Glove::sync_host_epoch,       py::arg("host_unix_ns"),
                                                                     call_guard<release_gil>())

        .def("__enter__", [](Glove& self) -> Glove& {
            bool ok;
            { py::gil_scoped_release release; ok = self.connect(); }
            if (!ok) throw NotConnectedError("TactileGlove: device not found");
            return self;
        })
        .def("__exit__", [](Glove& self, const py::object&,
                            const py::object&, const py::object&) {
            py::gil_scoped_release release;
            self.disconnect();
        });

    m.attr("BOOTLOADER_MAGIC") = BOOTLOADER_MAGIC;

    // Auto-generate __all__ from public attributes so update_stubs.py and the
    // tactile.py wrapper read a single source of truth — never hand-edited.
    py::list names;
    for (auto item : m.attr("__dict__").cast<py::dict>()) {
        std::string name = item.first.cast<std::string>();
        if (!name.empty() && name[0] != '_') names.append(name);
    }
    m.attr("__all__") = py::module_::import("builtins").attr("sorted")(names);
}

}  // namespace tactile_binding
