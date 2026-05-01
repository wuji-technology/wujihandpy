# Round-8 结构重构方案 — wujihandpy 视角

**Companion doc**: `wujihandros2/docs/refactor-plan.md`
**Status**: design frozen — execution in progress on `feat/tactile-ros2`
**Heads when frozen**: wujihandpy `05ad20a`, wujihandros2 `e2cd22c`

---

## 1. Why

Rounds 1–7 of code review covered correctness inside individual files
(concurrency, lifecycle, protocol compliance, CMake install export,
formatting). Round-8 is the first time anyone walked the full file tree
of both repos at once. Result: real structural problems neither rounds
1–7 nor codex's earlier passes ever addressed.

This document freezes:

- the rename / import / package maps,
- the alias policy (none — see §6),
- the 15-phase execution order with hard ordering constraints.

Subsequent commits should refer to the phase number in their message
header (e.g. `refactor(tactile): phase 6a — split wujihand_tactile_msgs`).

---

## 2. C++ rename map (this repo)

All tactile types move from `wujihandcpp::` (top level) into
`wujihandcpp::tactile::`. The `Tactile` prefix is dropped because it
duplicates the namespace.

### Types

| Old | New |
|---|---|
| `wujihandcpp::TactileBoard` | `wujihandcpp::tactile::Board` |
| `wujihandcpp::TactileFrame` | `wujihandcpp::tactile::Frame` |
| `wujihandcpp::TactileHandedness` | `wujihandcpp::tactile::Handedness` |
| `wujihandcpp::TactileDeviceInfo` | `wujihandcpp::tactile::DeviceInfo` |
| `wujihandcpp::TactileFwBuild` | `wujihandcpp::tactile::FwBuild` |
| `wujihandcpp::TactileDiagnostics` | `wujihandcpp::tactile::Diagnostics` |
| `wujihandcpp::TactileDeviceTime` | `wujihandcpp::tactile::DeviceTime` |
| `wujihandcpp::TactileSyncResult` | `wujihandcpp::tactile::SyncResult` |
| `wujihandcpp::TactileCmd` | `wujihandcpp::tactile::Cmd` |
| `wujihandcpp::TactileStatus` | `wujihandcpp::tactile::Status` |
| `wujihandcpp::TactileConfigKey` | `wujihandcpp::tactile::ConfigKey` |
| `wujihandcpp::TactileConfigType` | `wujihandcpp::tactile::ConfigType` |

### Exceptions

| Old | New |
|---|---|
| `wujihandcpp::TactileError` | `wujihandcpp::tactile::Error` |
| `wujihandcpp::TactileNotConnectedError` | `wujihandcpp::tactile::NotConnectedError` |
| `wujihandcpp::TactileWriteFailedError` | `wujihandcpp::tactile::WriteFailedError` |
| `wujihandcpp::TactileResponseTimeoutError` | `wujihandcpp::tactile::ResponseTimeoutError` |
| `wujihandcpp::TactileDisconnectedDuringRequestError` | `wujihandcpp::tactile::DisconnectedDuringRequestError` |
| `wujihandcpp::ConnectionLostError` | `wujihandcpp::tactile::ConnectionLostError` |

### Constants and free functions

| Old | New |
|---|---|
| `TACTILE_BOOTLOADER_MAGIC` | `wujihandcpp::tactile::BOOTLOADER_MAGIC` |
| `TACTILE_FRAME_MAX` | `wujihandcpp::tactile::FRAME_MAX` |
| `TACTILE_DEFAULT_TIMEOUT_MS` | `wujihandcpp::tactile::DEFAULT_TIMEOUT_MS` |
| `wujihandcpp::to_string(TactileStatus)` | `wujihandcpp::tactile::to_string(Status)` |

### Sub-namespace consolidation

| Old | New |
|---|---|
| `wujihandcpp::tactile_protocol::FRAME_SIZE` (and friends) | `wujihandcpp::tactile::protocol::FRAME_SIZE` |
| `wujihandcpp::tactile_protocol::crc16_ccitt(...)` | `wujihandcpp::tactile::protocol::crc16_ccitt(...)` |
| `wujihandcpp::tactile_protocol::parse_frame(...)` | `wujihandcpp::tactile::protocol::parse_frame(...)` |

### Constraint — no C++17 nested namespace shorthand

The repo runs a `cpp11_compat` CI check on public headers. Use the
explicit form to keep that gate green:

```cpp
// Allowed:
namespace wujihandcpp {
namespace tactile {
namespace protocol {
// ...
}  // namespace protocol
}  // namespace tactile
}  // namespace wujihandcpp

// Not allowed in public headers (C++17):
namespace wujihandcpp::tactile::protocol { ... }
```

---

## 3. Python import map

| Old | New |
|---|---|
| `from wujihandpy import TactileBoard` | `from wujihandpy.tactile import Board` |
| `from wujihandpy import TactileFrame, TactileError` | `from wujihandpy.tactile import Frame, Error` |
| `wujihandpy.TactileBoard` | `wujihandpy.tactile.Board` |
| `wujihandpy.TactileError` | `wujihandpy.tactile.Error` |

`wujihandpy.tactile` is a real Python module, not a pybind submodule
attribute. Implementation:

1. **pybind11**: `m.def_submodule("tactile", ...)` in `src/tactile.hpp`,
   classes registered without the `Tactile` prefix. This populates
   `wujihandpy._core.tactile`.
2. **Python wrapper**: a new `src/wujihandpy/tactile.py` re-exports from
   `wujihandpy._core.tactile`. This is what makes
   `import wujihandpy.tactile` work (otherwise users only get
   `wujihandpy._core.tactile`, which is a leaky abstraction).
3. **Stubs**: `update_stubs.py` regenerates `src/wujihandpy/_core/tactile.pyi`
   *and* `src/wujihandpy/tactile.pyi` (top-level wrapper stub) so type
   checkers resolve both paths.
4. **Top-level `__all__`** in `src/wujihandpy/__init__.py`: drop all 9
   `Tactile*` names; add `'tactile'`.

### Verification (Phase 3 acceptance)

All three must succeed:

```python
import wujihandpy.tactile
from wujihandpy import tactile
from wujihandpy.tactile import Board
```

---

## 4. example/ reorganization

Old (mixes "action" axis with "topic" axis):
```
example/
  1.read.py 2.write.py 3.realtime.py 4.async.py 5.multithread.py
  6.tactile.py
```

New:
```
example/
  joint/
    1.read.py 2.write.py 3.realtime.py 4.async.py 5.multithread.py
  tactile/
    basic.py
```

`tactile/basic.py` already lives at `example/6.tactile.py`; rename + fix
imports to use `wujihandpy.tactile.*`.

---

## 5. HIL test commit

`tests/hil_*.py` is currently `.gitignore`d. Phase 5 lifts that and
commits the SDK HIL suite.

- `.gitignore` rule changes: `tests/hil_*.py` → `tests/.local_*.py`
  (preserves the "local-only experiment" escape hatch).
- `hil_sdk.py` updated to use the new `wujihandpy.tactile.*` names.
- `WUJIHAND_HIL=1` env var gate at module import — without it the file
  exits 0 with a "skip: hardware required" message. Future pytest
  collectors will not auto-run it.
- New `tests/README.md` documents:
  - Hardware prerequisites (board flashed with firmware PR #30, TBIM
    written via `prodtest`).
  - How to invoke (`WUJIHAND_HIL=1 python tests/hil_sdk.py`).
  - Expected runtime (~15 s) and what it touches (`reset_counters`,
    `set_streaming(False/True)`, `enter_bootloader(bad_magic)`).

---

## 6. Alias policy

**No alias.** Hard break.

Justification:

- The new tactile API has never been released. Both PRs (#63 / #44) are
  Draft. The old/new `wujihandcpp::TactileBoard` shape only ever
  existed on `feat/tactile-ros2`.
- Only one in-tree downstream consumes this API: `wujihandros2`'s
  tactile driver, which lives in the sibling PR and is rewritten in the
  same change.
- Adding `[[deprecated]] using TactileBoard = tactile::Board;` would
  encourage external code to depend on the old name during a window
  where it never had a stable existence.

PR descriptions for both repos must call out this hard rename in a
"Breaking changes" section.

---

## 7. Execution plan (15 phases)

Phases are ordered by hard dependency. Hard-dep edges:

```
Phase 0  → all subsequent phases (design freeze)
Phase 1  → Phase 2 (binding needs new names)
Phase 2  → Phase 3 (.pyi regen needs binding final)
Phase 2  → Phase 5 (HIL uses new Python API)
Phase 6.0 → Phase 6a/b/c (CI must pull SDK head, not released deb)
Phase 1  → Phase 6b (driver uses new SDK namespace)
Phase 7.0 → Phase 7  (launch contract must normalize before reuse)
Phase 7  → Phase 8  (RViz overlay assumes composed launch)
```

| Phase | Repo | What | Acceptance |
|---|---|---|---|
| **0** | both | This document | committed |
| **1** | py | C++ rename: types + exceptions + constants + `tactile_protocol → tactile::protocol`. No C++17 shorthand. | `cmake --build` clean; `cpp11_compat` check green |
| **2** | py | pybind binding: `_core.tactile` submodule + `src/wujihandpy/tactile.py` wrapper | `import wujihandpy.tactile` succeeds at runtime |
| **3** | py | `update_stubs.py`; shrink top-level `__all__` to `[..., 'tactile']`; verify all three import paths | acceptance test in §3 passes; mypy/pyright resolves both stub paths |
| **4** | py | Reorganize `example/` into `joint/` + `tactile/` subdirs | each script runs (with hardware, manually) |
| **5** | py | Commit `hil_sdk.py`; `.gitignore` change; `WUJIHAND_HIL=1` gate; `tests/README.md` | `WUJIHAND_HIL=1 python tests/hil_sdk.py` 35/35 PASS |
| **6.0** | ros | CI workflow builds SDK from source (checkout sibling repo, `cmake --install`, then build ROS); update `build_deb.sh` deps; add missing `package.xml` runtime deps (`tf2_ros`, `ament_index_python`, `rcl_interfaces`) | CI green on a synthetic test PR before 6a |
| **6a** | ros | New package `wujihand_tactile_msgs`: 5 msg/srv files moved; old `wujihand_msgs/CMakeLists.txt` cleaned; new package builds standalone | `colcon build --packages-select wujihand_tactile_msgs` succeeds |
| **6b** | ros | New package `wujihand_tactile_driver`: tactile driver source moved, uses `wujihandcpp::tactile::*`, depends on `wujihand_tactile_msgs` | `colcon build --packages-up-to wujihand_tactile_driver` + binary runs |
| **6c** | ros | Joint package cleanup: `main.cpp` → `wujihand_driver_main.cpp`; remove tactile artifacts from `wujihand_driver/CMakeLists.txt` | `colcon build` whole workspace clean |
| **7.0** | ros | Normalize `tactile.launch.py` contract: arg-ize `parent_frame`, `serial_number`, `namespace`, `rviz`, `tactile_active` | `tactile.launch.py` runs standalone with explicit args; behavior unchanged |
| **7** | ros | `wujihand_full.launch.py` rewritten as `IncludeLaunchDescription(wujihand.launch.py) + IncludeLaunchDescription(tactile.launch.py)` with arg passthrough; USB auto-discovery moves to `common.py` | full launch behavior matches old monolithic version |
| **8** | ros | RViz overlay: delete `left_tactile.rviz` / `right_tactile.rviz`; add `tactile_overlay.json`; launch composes base + overlay into a tempfile (stdlib `json`); register shutdown cleanup | RViz shows tactile Image panel via launch arg; tempfile cleaned on Ctrl-C |
| **9a** | both | Sync docs: `docs/external/*.mdx`, READMEs, CHANGELOG entries reference new namespaces / packages / example paths | grep for old names returns 0 in user-facing docs |
| **9** | ros | Commit ROS HIL test as `tests/hil_driver.sh`; refresh both PR bodies | manual sanity run + pushed PR bodies show round-8 row |

---

## 8. CI / build / deb / docs sync checklist

Easy to forget; tracking inline:

- [ ] **Phase 1**: `cpp11_compat` test still passes (no C++17 namespace shorthand in public headers)
- [ ] **Phase 2**: pybind submodule registration order in `src/main.cpp`
- [ ] **Phase 3**: `update_stubs.py` script itself may need a tweak to traverse new submodule
- [ ] **Phase 6.0**: GitHub Actions workflow file (`.github/workflows/*.yml`) updated to checkout SDK at the right ref + cmake install
- [ ] **Phase 6.0**: `build_deb.sh` Debian Depends list updated
- [ ] **Phase 6a/b**: each new ROS package gets its own `package.xml` (Apache 2.0 license, deps declared)
- [ ] **Phase 7.0**: `tactile.launch.py` arg list documented in `CLAUDE.md`
- [ ] **Phase 9a**: `docs/external/{en,zh}/*.mdx` (4 files) grep for `TactileBoard` / `wujihand_msgs` and update
- [ ] **Phase 9a**: both `CHANGELOG.md` get a `## [unreleased]` round-8 section, not appended to existing tactile rewrite section

---

## 9. Top 3 risks (from codex round-8 plan review)

1. **Cross-repo CI version skew**: ROS CI today installs released
   `wujihandcpp.deb`, not the sibling SDK head. Phase 6 must be preceded
   by Phase 6.0 or every ROS commit between will be red.
2. **Python submodule semantics**: `m.def_submodule("tactile")` alone
   does **not** make `import wujihandpy.tactile` work — that needs the
   companion `src/wujihandpy/tactile.py` wrapper. Phase 2 acceptance
   test catches this.
3. **Launch / TF / RViz contract drift**: `IncludeLaunchDescription`
   exposes implicit contracts (parent_frame, namespace, tactile_active,
   serial discovery, RViz tempfile). Phase 7.0 normalizes these
   before Phase 7 actually composes the launches.
