from __future__ import annotations

from ._core import Finger, Hand, IController, Joint, filter
from ._version import __version__

# Tactile bindings are Linux-only — see wujihandcpp/include/wujihandcpp/data/
# tactile.hpp for the platform gate. Non-Linux wheels (Windows / macOS) ship
# `_core` without a `tactile` submodule because root CMakeLists.txt only
# defines WUJIHANDPY_ENABLE_TACTILE on `CMAKE_SYSTEM_NAME STREQUAL "Linux"`.
# The previous unconditional `from . import tactile` made `import wujihandpy`
# itself fail on those wheels with an opaque AttributeError on `_core.tactile`.
try:
    from . import tactile  # type: ignore[attr-defined]
    _HAS_TACTILE = True
except (ImportError, AttributeError):
    # No tactile bindings on this platform — leave the wrapper unimported
    # so the rest of the SDK is still usable.
    _HAS_TACTILE = False

__all__ = [
    "__version__",
    "Hand",
    "Finger",
    "Joint",
    "IController",
    "filter",
]
if _HAS_TACTILE:
    __all__.append("tactile")
