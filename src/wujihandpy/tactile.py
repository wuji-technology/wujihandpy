"""Tactile board API.

Re-exports the pybind11 submodule ``wujihandpy._core.tactile`` so that
``import wujihandpy.tactile`` and ``from wujihandpy.tactile import Board``
work without leaking the ``_core`` implementation detail to users.

Mirrors ``wujihandcpp::tactile`` on the C++ side.
"""

from __future__ import annotations

from ._core.tactile import (
    BOOTLOADER_MAGIC,
    Board,
    DeviceInfo,
    DeviceTime,
    Diagnostics,
    Error,
    Frame,
    FwBuild,
    Handedness,
    Status,
    SyncResult,
)

__all__ = [
    "BOOTLOADER_MAGIC",
    "Board",
    "DeviceInfo",
    "DeviceTime",
    "Diagnostics",
    "Error",
    "Frame",
    "FwBuild",
    "Handedness",
    "Status",
    "SyncResult",
]
