# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `Hand(side='left' | 'right')` constructor selects a wujihand by handedness without recording serial numbers. The SDK enumerates VID/PID matches, reads SDO `0x5090` (handedness) from each candidate, and connects to the unique match. `side` is keyword-only and mutually exclusive with `serial_number`. Failure cases (no device, side reversed, multiple same-side hands, candidate held by another Hand instance in this process) raise `ConnectionError` with an actionable message; per-device probe details are logged. See `example/joint/8.connect_by_side.py`.

## [1.7.0] - 2026-05-18

### Added

- **Tactile sensing glove support** (Linux only): top-level `wujihandpy.TactileGlove` plus typed companions `TactileFrame`, `TactileError`, `TactileHandedness`, and POD types for device info, diagnostics, firmware build, and time sync. Pressure frames are `numpy.float32` 24×32 arrays in `[0, 1]` with `NaN` for invalid cells. Hand and TactileGlove can coexist in one process—see `example/joint_with_tactile.py`.
- Added example `6.disconnect.py` demonstrating USB disconnect handling.
- Added example `joint/7.glove_donning.py` that smoothly drives the hand to a measured glove donning/doffing pose (thumb adducted across the palm, fingers nearly extended) at 100 Hz with low-pass filtering. Auto-selects the left- or right-hand pose via `read_handedness()`.

### Changed

- Changed `Hand` default `usb_pid` from `-1` to `0x2000`, to avoid silently matching the tactile sensing glove (shared VID `0x0483`). Pre-production firmware with other PIDs must pass `usb_pid=` explicitly.
- Changed Hand USB transport failures to raise `ConnectionError` (was `RuntimeError`), matching `TactileGlove`. The same `wujihandcpp::device::ConnectionError` (Python `ConnectionError`) is now also raised when the device disconnects mid-runtime — covering blocking SDO calls, in-flight async reads, and raw SDO operations. Pending async callbacks are explicitly woken so callers don't hang. See `example/joint/6.disconnect.py` for the recommended catch pattern.
  - Note: in realtime control mode, `IController.get_joint_actual_position` / `set_joint_target_position` go through PDO atomic operations and do NOT raise on disconnect. To detect disconnect inside a realtime loop, periodically issue a SDO probe (e.g. `hand.read_input_voltage()`).
- Changed `TactileGlove()` without `serial_number` to raise `ConnectionError` listing found serials when multiple tactile sensing gloves are on the bus, instead of silently picking the first.
- Updated CMake integration for C++ consumers to use `find_package(wujihandcpp CONFIG REQUIRED)` + `wujihandcpp::wujihandcpp`.
- Reorganized examples into `example/joint/`, `example/tactile/basic.py`, and `example/joint_with_tactile.py`.

### Removed

- Removed the `@control` acquire/release protocol from the Zenoh Bridge (Python + C++); SET / PUT writes no longer require a handshake.

### Fixed

- Fixed Linux release build (`Dockerfile.package-builder` / cibuildwheel) failing to link `wujihandcpp_tests` with `undefined reference to wujihandcpp::tactile::FrameDemuxer::*`. Root cause was `FrameDemuxer`'s symbols being stripped from `libwujihandcpp.so` by `-fvisibility=hidden` (it's a `src/`-private class with no `visibility("default")` annotation). The test target now compiles `src/device/frame_demuxer.cpp` directly into the executable on Linux + shared-library builds, leaving the library's public ABI unchanged.

## [1.6.0] - 2026-04-27

### Changed

- Initialization failure now reports specific disconnected joints (e.g. `finger(2).joint(1)`) instead of generic error message
- **Zenoh Bridge (Python)**: `realtime_controller` LowPass cutoff is now configurable via `--filter-cutoff` (default `5.0` Hz, matching `example/3.realtime.py`); previously hard-coded at 10000 Hz. Pass `--filter-cutoff 10000` to restore the prior near-passthrough behavior.
- **Zenoh Bridge (Python)**: `--side {left,right}` is now a required CLI argument so the published `joint_states` joint names match the URDF loaded downstream.

### Added

- **Firmware upgrade reminder**: `Hand()` now displays an in-terminal banner with the latest version and a link to the upgrade guide whenever your device firmware is out of date
- **Zenoh Bridge (Python)**: standalone bridge process exposing WujiHand via Zenoh network protocol (`bridge/python/hand_zenoh_bridge.py`)
- **Zenoh Bridge (C++)**: native C++ bridge with lower latency for production deployment (`bridge/cpp/`)
- 16 Zenoh resources: 12 GET (scalar + 5×4 joint arrays), 5 SET (target_position, control_mode, enabled, effort_limit, reset_error)
- 2 SUB publishers (actual_position + actual_effort) with configurable `--pub-rate` (no default, must be explicitly set)
- Host-side UTC microsecond timestamps in `{timestamp_us, data}` envelope format for all SUB data
- `@capability` queryable with full JSON schema (SUB resources include timestamp envelope schema)
- `@control` acquire/release protocol with liveliness-based TTL for automatic crash recovery
- Realtime controller integration: target_position writes via atomic update → PDO 1kHz
- Python/C++ fire-and-forget target_position subscriber for low-latency PUT writes
- 37 unit tests for bridge protocol, resources, timestamps, and control ownership
- `bridge/README.md` with architecture, usage, and resource documentation
- **Zenoh Bridge (Python)**: `joint_states` SUB topic (`sensor_msgs/JointState`) — flat row-major projection of `joint/actual_position` with joint names matching [`wuji-hand-description`](https://github.com/wuji-technology/wuji-hand-description) URDFs, enabling live URDF visualization in Wuji Studio's 3D panel. Published without the timestamp envelope so the schema title stays exactly `sensor_msgs/JointState`; ordering is carried in `header.stamp` via a bridge-side monotonic clock.

## [1.5.1] - 2026-02-02

### Added

- `Hand.disable_thread_safe_check()` API for multi-threaded usage
- Example: `5.multithread.py` demonstrating multi-threaded PDO operations

## [1.5.0] - 2026-01-19

### Added

- Real-time `joint_effort` reading via `IController.get_joint_actual_effort()`
- `joint_effort_limit` now supports read operations (previously write-only)
- Example: add `read_joint_effort_limit()` demo in `1.read.py`

### Changed

- Effort values use Ampere (A) units externally, with automatic mA conversion for firmware
- Renamed `current_limit` to `effort_limit` across all APIs

### Deprecated

- `current_limit` API - use `effort_limit` instead

## [1.4.0] - 2025-12-19

### Added

- Serial number (SN) reading from device via `read_product_sn()`
- Automatic exception detection with remediation hints based on TPDO error codes

### Fixed

- C++11 compatibility for header files
- Version parsing for release candidate (rc) versions

### Changed

- Refactored examples for improved clarity
- Enhanced README with bilingual support (English/Chinese)

## [1.3.0] - 2025-12-05

### Added

- Firmware-side real-time filtering support
- Full system firmware version reporting
- Automatic SDK/firmware version logging during initialization
- Protocol-level latency testing

### Changed

- Unified version naming: `hardware_version/date` → `firmware_version/date`
- C++ `Hand::realtime_controller` now returns `std::unique_ptr<IController>` with filter support

### Removed

- `attach_realtime_controller()` and `detach_realtime_controller()` (C++ API only)

## [1.2.0] - 2025-11-07

### Added

- Interface timeout configuration (default 0.5s)
- Logging system

### Changed

- New `realtime_controller()` interface with built-in low-pass filter
  - Automatically switches to real-time control mode on enter
  - Restores Point-to-Point mode on exit
  - Enables stable 1kHz control with low-frequency (~20-100 Hz) input
- Optimized joint data naming:
  - `joint_control_word` → `joint_enabled`
  - `joint_position` → `joint_actual_position`
  - `joint_control_position` → `joint_target_position`
- Removed mandatory NumPy type requirement for input parameters

### Deprecated

- `pdo_write_unchecked()` - use `realtime_controller()` instead

### Compatibility

- Requires firmware v3.0.0+

[Unreleased]: https://github.com/wuji-technology/wujihandpy/compare/v1.7.0...HEAD
[1.7.0]: https://github.com/wuji-technology/wujihandpy/compare/v1.6.0...v1.7.0
[1.6.0]: https://github.com/wuji-technology/wujihandpy/compare/v1.5.1...v1.6.0
[1.5.1]: https://github.com/wuji-technology/wujihandpy/compare/v1.5.0...v1.5.1
[1.5.0]: https://github.com/wuji-technology/wujihandpy/compare/v1.4.0...v1.5.0
[1.4.0]: https://github.com/wuji-technology/wujihandpy/compare/v1.3.0...v1.4.0
[1.3.0]: https://github.com/wuji-technology/wujihandpy/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/wuji-technology/wujihandpy/releases/tag/v1.2.0
