# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Removed

- **Zenoh Bridge (Python + C++)**: removed `@control` acquire/release protocol
  and the liveliness-based owner TTL watcher. Writes (SET resources,
  fire-and-forget `joint/target_position` PUT) no longer require an acquire
  handshake or a requester-identity attachment — any client reachable over
  Zenoh may write. Single-writer protection, if needed, must be enforced by
  the deployment topology (firewall rules, Zenoh ACL, isolated network).

  Aligns with `wuji-sdk` PR #215 [SWD-1132], which removed the corresponding
  `acquire_control` / `release_control` SDK API and per-write attachment
  plumbing. Without this companion change, `wuji-sdk` clients would lose
  the ability to write WujiHand via this bridge.

  Removed (Python): `_handle_control` / `_get_requester_id` /
  `_start_owner_watcher` / `_stop_owner_watcher` / `_control_owner_key` /
  `_control_owner` / `_control_lock` / `_control_owner_watcher` /
  `decode_zenoh_text` (now unused).
  Removed (C++): `handle_control` / `start_owner_watcher` /
  `stop_owner_watcher` / `control_owner_key` / `control_owner_` /
  `control_mutex_` / `control_owner_watcher_` / `requester_id_from_attachment`.
  Tests: 14 control-권 / attachment / owner-watcher unit tests removed,
  replaced with one positive test confirming SET succeeds without an
  attachment. README "Control Protocol" section replaced with a "Write
  Access" note explaining the new policy.

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

[Unreleased]: https://github.com/wuji-technology/wujihandpy/compare/v1.6.0...HEAD
[1.6.0]: https://github.com/wuji-technology/wujihandpy/compare/v1.5.1...v1.6.0
[1.5.1]: https://github.com/wuji-technology/wujihandpy/compare/v1.5.0...v1.5.1
[1.5.0]: https://github.com/wuji-technology/wujihandpy/compare/v1.4.0...v1.5.0
[1.4.0]: https://github.com/wuji-technology/wujihandpy/compare/v1.3.0...v1.4.0
[1.3.0]: https://github.com/wuji-technology/wujihandpy/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/wuji-technology/wujihandpy/releases/tag/v1.2.0
