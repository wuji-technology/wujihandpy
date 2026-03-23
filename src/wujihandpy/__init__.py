from __future__ import annotations

from ._core import *

try:
    from ._version import __version__
except ImportError:
    __version__ = "0+unknown"

__all__ = ["__version__", 'Hand', 'Finger', 'Joint', 'IController', 'filter']
