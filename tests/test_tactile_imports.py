"""Tactile module import-surface invariants — no hardware needed.

These run in CI on every PR and lock the public API shape so a future
edit to the C++ binding can't silently desync the wrapper / stub.

Platform handling: the tactile bindings are Linux-only (root
CMakeLists.txt gates WUJIHANDPY_ENABLE_TACTILE on CMAKE_SYSTEM_NAME==
"Linux"). On non-Linux this test file is SKIPPED at module level —
`_core` ships without the tactile submodule there. On Linux, however,
the tactile binding MUST exist; importorskip-everywhere would silently
pass on a regression where the binding accidentally fails to compile
or is mis-built. Use a platform-conditional skip so Linux runs see
import errors as test failures, not "skipped — nothing to do".
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
    """`from wujihandpy.tactile import Board`,
       `from wujihandpy import tactile; tactile.Board`,
       `import wujihandpy.tactile; wujihandpy.tactile.Board` —
    all three must resolve to the same class object."""
    from wujihandpy.tactile import Board as B1
    from wujihandpy import tactile
    B2 = tactile.Board
    import wujihandpy.tactile as t3
    B3 = t3.Board
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
    must_have = {"Board", "Frame", "Error", "BOOTLOADER_MAGIC"}
    assert must_have.issubset(set(mod.__all__))


def test_top_level_all_includes_tactile():
    """`wujihandpy.__all__` should advertise `tactile` so it's
    discoverable via dir() / IDEs."""
    assert "tactile" in wujihandpy.__all__


def test_exception_hierarchy():
    """tactile.Error is the base class for tactile-status exceptions."""
    assert issubclass(wujihandpy.tactile.Error, Exception)
