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
import wujihandpy.tactile  # noqa: E402
import wujihandpy._core.tactile  # noqa: E402


def test_wrapper_all_matches_native():
    """src/wujihandpy/tactile.py is now `from ._core.tactile import *`
    + `__all__ = list(_tactile_module.__all__)` — verify the runtime
    surface matches what the native module exports."""
    wrapper = set(wujihandpy.tactile.__all__)
    native = set(wujihandpy._core.tactile.__all__)
    assert wrapper == native, (
        f"wrapper.__all__ drifted from _core.tactile.__all__:\n"
        f"  wrapper-only: {sorted(wrapper - native)}\n"
        f"  native-only:  {sorted(native - wrapper)}"
    )


def test_three_import_paths_resolve_identically():
    """`from wujihandpy.tactile import Glove`,
       `from wujihandpy import tactile; tactile.Glove`,
       `import wujihandpy.tactile; wujihandpy.tactile.Glove` —
    all three must resolve to the same class object."""
    from wujihandpy.tactile import Glove as B1
    from wujihandpy import tactile
    B2 = tactile.Glove
    import wujihandpy.tactile as t3
    B3 = t3.Glove
    assert B1 is B2 is B3


def test_native_all_is_populated():
    """Regression guard for the bug round-1 found: pybind11 doesn't
    auto-add __all__ to def_submodule()'d modules. tactile.hpp now
    sets it explicitly from the module's own __dict__; this test
    locks that behavior."""
    mod = wujihandpy._core.tactile
    assert hasattr(mod, "__all__"), \
        "_core.tactile.__all__ missing — wrapper.tactile.py would break"
    # Sanity check: __all__ must contain at least the headline classes.
    must_have = {"Glove", "Frame", "Error", "BOOTLOADER_MAGIC"}
    assert must_have.issubset(set(mod.__all__))


def test_top_level_all_includes_tactile():
    """`wujihandpy.__all__` should advertise `tactile` so it's
    discoverable via dir() / IDEs."""
    assert "tactile" in wujihandpy.__all__


def test_exception_hierarchy():
    """tactile.Error is the base class for tactile-status exceptions."""
    assert issubclass(wujihandpy.tactile.Error, Exception)
