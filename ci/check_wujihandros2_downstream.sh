#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 /path/to/wujihandcpp.deb" >&2
  exit 2
fi

if [[ -z "${ROS_DISTRO:-}" ]]; then
  echo "ROS_DISTRO must be set" >&2
  exit 2
fi

deb_package="$1"
if [[ ! -f "${deb_package}" || "${deb_package}" != *.deb ]]; then
  echo "deb package not found: ${deb_package}" >&2
  exit 2
fi

export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends \
  build-essential \
  ca-certificates \
  cmake \
  git \
  pkg-config \
  python3-colcon-common-extensions \
  python3-rosdep

apt-get install -y "${deb_package}"
dpkg -s wujihandcpp

installed_files="$(mktemp)"
dpkg -L wujihandcpp | tee "${installed_files}"
if grep -E '(^|/)spdlog(/|$)|(^|/)libspdlog|(^|/)spdlog[.]pc$|(^|/)cmake/spdlog(/|$)' "${installed_files}"; then
  echo "wujihandcpp deb unexpectedly installed spdlog files" >&2
  exit 1
fi

cmake_dirs=()
while IFS= read -r dir; do
  cmake_dirs+=("${dir}")
done < <(find /usr/lib /usr/lib64 -type d -path '*/cmake/wujihandcpp' 2>/dev/null | sort)
if [[ "${#cmake_dirs[@]}" -eq 0 ]]; then
  echo "wujihandcpp CMake package directory not found" >&2
  exit 1
fi
if grep -R 'spdlog::spdlog' "${cmake_dirs[@]}"; then
  echo "wujihandcpp CMake export unexpectedly exposes spdlog::spdlog" >&2
  exit 1
fi

sdk_lib="$(find /usr/lib /usr/lib64 -type f -name 'libwujihandcpp.so*' 2>/dev/null | sort | head -n 1)"
if [[ -z "${sdk_lib}" ]]; then
  echo "libwujihandcpp.so not found after installing deb" >&2
  exit 1
fi
ldd "${sdk_lib}"
if ldd "${sdk_lib}" | grep -E 'not found'; then
  echo "libwujihandcpp.so has unresolved dynamic dependencies" >&2
  exit 1
fi

consumer_dir="$(mktemp -d)"
cat >"${consumer_dir}/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.16)
project(wujihandcpp_deb_consumer LANGUAGES CXX)

find_package(wujihandcpp CONFIG REQUIRED)

add_executable(wujihandcpp_deb_consumer main.cpp)
target_compile_features(wujihandcpp_deb_consumer PRIVATE cxx_std_11)
target_link_libraries(wujihandcpp_deb_consumer PRIVATE wujihandcpp::wujihandcpp)
CMAKE
cat >"${consumer_dir}/main.cpp" <<'CPP'
#include <wujihandcpp/data/joint.hpp>
#include <wujihandcpp/utility/logging.hpp>

int main() {
    wujihandcpp::logging::set_log_to_console(false);
    wujihandcpp::logging::set_log_level(wujihandcpp::logging::Level::OFF);
    constexpr auto index = wujihandcpp::data::joint::TargetPosition::index;
    return index == 0x7A ? 0 : 1;
}
CPP
cmake -S "${consumer_dir}" -B "${consumer_dir}/build"
cmake --build "${consumer_dir}/build" --parallel 2
"${consumer_dir}/build/wujihandcpp_deb_consumer"

rosdep init 2>/dev/null || true
rosdep update --rosdistro "${ROS_DISTRO}"

workspace="$(mktemp -d)"
mkdir -p "${workspace}/src"
git clone --recurse-submodules --depth 1 \
  "${WUJIHANDROS2_REPO:-https://github.com/wuji-technology/wujihandros2.git}" \
  "${workspace}/src/wujihandros2"

cd "${workspace}"
rosdep install \
  --from-paths src \
  --ignore-src \
  --rosdistro "${ROS_DISTRO}" \
  --skip-keys wujihandcpp \
  -r -y

# shellcheck disable=SC1090
. "/opt/ros/${ROS_DISTRO}/setup.bash"
colcon build --event-handlers console_direct+
