#include <type_traits>

#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <wujihandcpp/device/finger.hpp>
#include <wujihandcpp/device/hand.hpp>
#include <wujihandcpp/device/joint.hpp>
#include <wujihandcpp/device/latch.hpp>

#include "controller.hpp"
#include "filter.hpp"
#include "logging.hpp"
#include "wrapper.hpp"

namespace py = pybind11;

template <typename Data>
void register_py_interface(const std::string&) {}

template <typename Data, typename T, typename... Others>
void register_py_interface(const std::string& name, py::class_<T>& py_class, Others&... others) {
    T::template register_py_interface<Data>(
        py_class,
        std::is_same_v<typename Data::Base, wujihandcpp::device::Joint> ? "joint_" + name : name);
    register_py_interface<Data>(name, others...);
}

PYBIND11_MODULE(_core, m) {
    py::register_exception_translator([](std::exception_ptr p) {
        if (!p)
            return;
        try {
            std::rethrow_exception(p);
        } catch (const wujihandcpp::device::TimeoutError& e) {
            PyErr_SetString(PyExc_TimeoutError, e.what());
        }
    });

    py::class_<IControllerWrapper>(m, "IController")
        .def("__enter__", [](IControllerWrapper& self) -> IControllerWrapper& { return self; })
        .def(
            "__exit__", [](IControllerWrapper& self, const py::object&, const py::object&,
                           const py::object&) { self.close(); })
        .def("close", &IControllerWrapper::close)
        .def("get_joint_actual_position", &IControllerWrapper::get_joint_actual_position)
        .def("get_joint_actual_effort", &IControllerWrapper::get_joint_actual_effort)
        .def(
            "set_joint_target_position", &IControllerWrapper::set_joint_target_position,
            py::arg("value_array"));

    filter::init_module(m);

    logging::init_module(m);

    using namespace wujihandcpp;

    using Hand = Wrapper<wujihandcpp::device::Hand>;
    auto hand = py::class_<Hand>(m, "Hand");
    hand.def(
        py::init<std::optional<std::string>, int32_t, uint16_t, std::optional<py::array_t<bool>>>(),
        py::arg("serial_number") = py::none(), py::arg("usb_pid") = -1, py::arg("usb_vid") = 0x0483,
        py::arg("mask") = py::none());

    register_py_interface<data::hand::Handedness>("handedness", hand);
    register_py_interface<data::hand::FirmwareVersion>("firmware_version", hand);
    register_py_interface<data::hand::FirmwareDate>("firmware_date", hand);
    register_py_interface<data::hand::SystemTime>("system_time", hand);
    register_py_interface<data::hand::Temperature>("temperature", hand);
    register_py_interface<data::hand::InputVoltage>("input_voltage", hand);

    hand.def(
        "realtime_controller", &Hand::realtime_controller, py::arg("enable_upstream"),
        py::arg("filter"), py::keep_alive<0, 1>());

    hand.def("start_latency_test", &Hand::start_latency_test);
    hand.def("stop_latency_test", &Hand::stop_latency_test);

    // Raw SDO operations for debugging
    hand.def(
        "raw_sdo_read", &Hand::raw_sdo_read, py::arg("finger_id"), py::arg("joint_id"),
        py::arg("index"), py::arg("sub_index"), py::arg("timeout") = 0.5);
    hand.def(
        "raw_sdo_write", &Hand::raw_sdo_write, py::arg("finger_id"), py::arg("joint_id"),
        py::arg("index"), py::arg("sub_index"), py::arg("data"), py::arg("timeout") = 0.5);

    // Product SN
    hand.def(
        "get_product_sn", &Hand::get_product_sn,
        "Get device product serial number");

    using Finger = Wrapper<wujihandcpp::device::Finger>;
    auto finger = py::class_<Finger>(m, "Finger");
    hand.def("finger", &Hand::finger, py::arg("index"), py::keep_alive<0, 1>());

    using Joint = Wrapper<wujihandcpp::device::Joint>;
    auto joint = py::class_<Joint>(m, "Joint");
    finger.def("joint", &Finger::joint, py::arg("index"), py::keep_alive<0, 1>());

    register_py_interface<data::joint::FirmwareVersion>("firmware_version", hand, finger, joint);
    register_py_interface<data::joint::FirmwareDate>("firmware_date", hand, finger, joint);
    register_py_interface<data::joint::ControlMode>("control_mode", hand, finger, joint);
    register_py_interface<data::joint::SinLevel>("sin_level", hand, finger, joint);
    register_py_interface<data::joint::EffortLimit>("effort_limit", hand, finger, joint);
    register_py_interface<data::joint::CurrentLimit>("current_limit", hand, finger, joint);  // deprecated, use effort_limit
    register_py_interface<data::joint::BusVoltage>("bus_voltage", hand, finger, joint);
    register_py_interface<data::joint::Temperature>("temperature", hand, finger, joint);
    register_py_interface<data::joint::ResetError>("reset_error", hand, finger, joint);
    register_py_interface<data::joint::ErrorCode>("error_code", hand, finger, joint);
    register_py_interface<data::joint::Enabled>("enabled", hand, finger, joint);
    register_py_interface<data::joint::ActualPosition>("actual_position", hand, finger, joint);
    register_py_interface<data::joint::TargetPosition>("target_position", hand, finger, joint);
    register_py_interface<data::joint::UpperLimit>("upper_limit", hand, finger, joint);
    register_py_interface<data::joint::LowerLimit>("lower_limit", hand, finger, joint);
}
