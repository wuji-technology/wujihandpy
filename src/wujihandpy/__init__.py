from __future__ import annotations

from ._core import Finger, Hand, IController, Joint, filter, logging
from ._version import __version__

# Tactile bindings are Linux-only; keep the rest of wujihandpy importable elsewhere.
try:
    from . import tactile  # type: ignore[attr-defined]
    _HAS_TACTILE = True
except (ImportError, AttributeError):
    _HAS_TACTILE = False

__all__ = [
    "__version__",
    "Hand",
    "Finger",
    "Joint",
    "IController",
    "filter",
    "logging",
]
if _HAS_TACTILE:
    __all__.append("tactile")
