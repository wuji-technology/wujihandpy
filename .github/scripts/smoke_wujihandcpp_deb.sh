#!/usr/bin/env bash
set -euo pipefail

deb_package="${1:?usage: smoke_wujihandcpp_deb.sh /path/to/wujihandcpp.deb [install-system-spdlog]}"
install_system_spdlog="${2:-false}"

deb_package="$(realpath "${deb_package}")"
if [[ ! -f "${deb_package}" ]]; then
  echo "deb package does not exist: ${deb_package}" >&2
  exit 1
fi

if [[ "${deb_package}" != *.deb ]]; then
  echo "expected a .deb package, got: ${deb_package}" >&2
  exit 1
fi

if [[ "${install_system_spdlog}" != "true" && "${install_system_spdlog}" != "false" ]]; then
  echo "install-system-spdlog must be true or false, got: ${install_system_spdlog}" >&2
  exit 1
fi

if [[ "${EUID}" -eq 0 ]]; then
  sudo_cmd=()
else
  sudo_cmd=(sudo)
fi

export DEBIAN_FRONTEND=noninteractive

"${sudo_cmd[@]}" apt-get update
if dpkg-query -W -f='${Status}' wujihandcpp 2>/dev/null | grep -q "install ok installed"; then
  "${sudo_cmd[@]}" apt-get purge -y wujihandcpp
fi
if [[ "${install_system_spdlog}" == "false" ]]; then
  if dpkg-query -W -f='${Status}' libspdlog-dev 2>/dev/null | grep -q "install ok installed"; then
    "${sudo_cmd[@]}" apt-get purge -y libspdlog-dev
  fi
fi

"${sudo_cmd[@]}" apt-get install -y --no-install-recommends \
  ca-certificates \
  cmake \
  g++ \
  make \
  pkg-config

if [[ "${install_system_spdlog}" == "true" ]]; then
  "${sudo_cmd[@]}" apt-get install -y --no-install-recommends libspdlog-dev
fi

"${sudo_cmd[@]}" apt-get install -y --no-install-recommends "${deb_package}"

if dpkg -L wujihandcpp | grep -E '(^|/)(include/spdlog|libspdlog|cmake/spdlog|pkgconfig/spdlog\.pc)'; then
  echo "wujihandcpp deb unexpectedly installs spdlog-owned files" >&2
  exit 1
fi

consumer_dir="$(mktemp -d)"
trap 'rm -rf "${consumer_dir}"' EXIT

cat >"${consumer_dir}/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(wujihandcpp_deb_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(wujihandcpp REQUIRED)

add_executable(wujihandcpp_deb_consumer main.cpp)
target_link_libraries(wujihandcpp_deb_consumer PRIVATE wujihandcpp::wujihandcpp)
CMAKE

cat >"${consumer_dir}/main.cpp" <<'CPP'
#include <iostream>
#include <string>

#include <wujihandcpp/data/helper.hpp>
#include <wujihandcpp/filter/low_pass.hpp>
#include <wujihandcpp/utility/logging.hpp>

int main() {
    const wujihandcpp::data::FirmwareVersionData version{1, 2, 3};
    const std::string version_string = version.to_string();
    if (version_string.rfind("1.2.3", 0) != 0) {
        std::cerr << "unexpected version string: " << version_string << "\n";
        return 1;
    }

    wujihandcpp::filter::LowPass filter{5.0};
    filter.setup(100.0);
    wujihandcpp::filter::LowPass::Unit unit;
    unit.reset(filter, 0.0);
    unit.input(filter, 1.0);
    if (unit.step(filter) <= 0.0) {
        std::cerr << "low pass filter did not advance\n";
        return 2;
    }

    wujihandcpp::logging::set_log_to_console(false);
    wujihandcpp::logging::set_log_to_file(false);
    wujihandcpp::logging::log(
        wujihandcpp::logging::Level::INFO,
        "deb consumer smoke",
        std::string("deb consumer smoke").size());
    wujihandcpp::logging::flush();
    return 0;
}
CPP

cmake -S "${consumer_dir}" -B "${consumer_dir}/build"
cmake --build "${consumer_dir}/build" --verbose
"${consumer_dir}/build/wujihandcpp_deb_consumer"
