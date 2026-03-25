# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **TouchBoard** device class: USB CDC connection to tboard (STM32H723) for tactile data
  - `TouchBoard()` — connect via libusb (PID=0x5700)
  - `read_tactile()` / `read_tactile_raw()` — blocking read with timeout
  - `get_tactile()` / `get_tactile_raw()` — non-blocking latest frame
  - `.handedness`, `.fps`, `.frame_count` read-only properties
  - C++ TactileParser: frame sync state machine + CRC16-CCITT validation
  - USB transport: configurable interface/endpoint support
- **Zenoh Bridge module** (`wujihandpy.bridge`, optional `[bridge]` extra)
  - `HandBridge`: publishes Hand joint data to Zenoh with resource model, control ownership, and realtime loop
  - `TactileBridge`: publishes TouchBoard tactile data to Zenoh at configurable rate
  - CLI entry points: `wujihandpy-bridge`, `wujihandpy-tactile-bridge`
  - `build_capability()`, `get_timestamp_us()`, `sanitize_sn()`, `wrap_with_timestamp()` utilities
  - 16 Zenoh resources: 12 GET + 5 SET, 2 SUB publishers with configurable `--pub-rate`
  - `@capability` queryable with JSON schema, `@control` acquire/release with liveliness TTL
  - Realtime controller integration: target_position via atomic update → PDO 1kHz
- Integration demo script (`examples/integration_demo.py`)
- Python package CI workflow (`.github/workflows/python-package-ci.yml`)
- Unit tests for bridge module (50 tests in `tests/test_bridge.py`)

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

[Unreleased]: https://github.com/wuji-technology/wujihandpy/compare/v1.5.1...HEAD
[1.5.1]: https://github.com/wuji-technology/wujihandpy/compare/v1.5.0...v1.5.1
[1.5.0]: https://github.com/wuji-technology/wujihandpy/compare/v1.4.0...v1.5.0
[1.4.0]: https://github.com/wuji-technology/wujihandpy/compare/v1.3.0...v1.4.0
[1.3.0]: https://github.com/wuji-technology/wujihandpy/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/wuji-technology/wujihandpy/releases/tag/v1.2.0
