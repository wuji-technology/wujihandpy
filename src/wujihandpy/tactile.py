"""Public re-export of the native tactile submodule."""

from __future__ import annotations

from wujihandpy._core import tactile as _tactile_module
from wujihandpy._core.tactile import *  # noqa: F401,F403

__all__ = list(_tactile_module.__all__)
