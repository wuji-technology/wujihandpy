"""Tactile import-surface invariants; no hardware needed.

Skip only on non-Linux. On Linux, missing tactile bindings are failures.
"""
from __future__ import annotations

import sys

import pytest

if not sys.platform.startswith("linux"):
    pytest.skip(
        "tactile bindings are Linux-only (see wujihandcpp/include/"
        "wujihandcpp/data/tactile.hpp for the platform gate)",
        allow_module_level=True,
    )

# Hard import on Linux: regressions in the binding must fail this file,
# not silently skip it.
import wujihandpy  # noqa: E402
import wujihandpy._core.tactile  # noqa: E402


# Public names re-exported from wujihandpy._core.tactile to wujihandpy.*
# with the `Tactile` prefix. Single source of truth for the rest of the
# tests in this file.
TACTILE_FLAT_NAMES = {
    "Glove": "TactileGlove",
    "Frame": "TactileFrame",
    "Handedness": "TactileHandedness",
    "Status": "TactileStatus",
    "Error": "TactileError",
    "DeviceInfo": "TactileDeviceInfo",
    "FwBuild": "TactileFwBuild",
    "Diagnostics": "TactileDiagnostics",
    "DeviceTime": "TactileDeviceTime",
    "SyncResult": "TactileSyncResult",
    "BOOTLOADER_MAGIC": "TACTILE_BOOTLOADER_MAGIC",
}


def test_flat_reexports_match_native():
    """Each native `_core.tactile.X` must be re-exported as
    `wujihandpy.TactileX` (or `TACTILE_X` for constants), and they must
    be the *same object* — not a copy or wrapper."""
    native = wujihandpy._core.tactile
    for src, dst in TACTILE_FLAT_NAMES.items():
        assert hasattr(wujihandpy, dst), f"wujihandpy.{dst} missing"
        assert getattr(wujihandpy, dst) is getattr(native, src), (
            f"wujihandpy.{dst} is not the same object as "
            f"wujihandpy._core.tactile.{src}"
        )


def test_native_all_is_populated():
    """Regression guard: pybind11 doesn't auto-add __all__ to
    def_submodule()'d modules. tactile.hpp sets it explicitly from the
    module's own __dict__ — this test locks that behavior because the
    flat re-export in __init__.py relies on the native names being
    present."""
    mod = wujihandpy._core.tactile
    assert hasattr(mod, "__all__"), \
        "_core.tactile.__all__ missing — flat re-export would silently drift"
    must_have = {"Glove", "Frame", "Error", "BOOTLOADER_MAGIC"}
    assert must_have.issubset(set(mod.__all__))


def test_top_level_all_includes_flat_names():
    """`wujihandpy.__all__` should advertise every flat tactile name so
    they're discoverable via dir() / IDE completion."""
    top_all = set(wujihandpy.__all__)
    for dst in TACTILE_FLAT_NAMES.values():
        assert dst in top_all, f"wujihandpy.__all__ missing {dst!r}"


def test_no_legacy_tactile_submodule():
    """The `wujihandpy.tactile` submodule was retired in favor of flat
    `wujihandpy.TactileX` names. Importing it must fail cleanly so users
    discover the new spelling instead of falling through to a stale
    cached attribute."""
    with pytest.raises(ImportError):
        import wujihandpy.tactile  # noqa: F401


def test_exception_hierarchy():
    """TactileError is the base class for tactile-status exceptions."""
    assert issubclass(wujihandpy.TactileError, Exception)
