from __future__ import annotations

import sys
from typing import TYPE_CHECKING, Optional, SupportsIndex

# `Annotated` only entered `typing` in 3.9; pull it from the
# `typing_extensions` backport on 3.8 so type annotations referring to it
# can be resolved at runtime (e.g. `typing.get_type_hints(Hand.__init__)`)
# on every supported interpreter.
if sys.version_info >= (3, 9):
    from typing import Annotated
else:
    from typing_extensions import Annotated

from . import _core
# `filter` and `logging` are wujihandpy submodules; the same-name shadowing
# of Python builtins is intentional and part of the public API surface.
from ._core import Finger, Joint, IController, filter, logging  # noqa: F401, A004
from ._upgrade_check import trigger_check_in_background
from ._version import __version__

# Tactile bindings are Linux-only and gated by WUJIHANDPY_ENABLE_TACTILE at
# build time. Catch only "the native tactile submodule isn't on this wheel" —
# anything else (ABI mismatch, missing transitive dep, syntax error in an
# imported file) must propagate so users see the real failure instead of
# silently losing the API surface.
try:
    from ._core.tactile import (  # noqa: F401
        BOOTLOADER_MAGIC as TACTILE_BOOTLOADER_MAGIC,
        DeviceInfo as TactileDeviceInfo,
        DeviceTime as TactileDeviceTime,
        Diagnostics as TactileDiagnostics,
        Error as TactileError,
        Frame as TactileFrame,
        FwBuild as TactileFwBuild,
        Glove as TactileGlove,
        Handedness as TactileHandedness,
        Status as TactileStatus,
        SyncResult as TactileSyncResult,
    )
except ModuleNotFoundError as _tactile_err:
    if _tactile_err.name != f"{__name__}._core.tactile":
        raise
    _HAS_TACTILE = False
else:
    _HAS_TACTILE = True

if TYPE_CHECKING:
    import numpy
    import numpy.typing


class Hand(_core.Hand):
    """Hand with automatic background firmware-upgrade check.

    The check runs in a daemon thread after __init__ returns; any failure
    is silently absorbed so Hand() behaves identically to the underlying
    C++ binding.
    """

    def __init__(
        self,
        serial_number: Optional[str] = None,
        usb_pid: SupportsIndex = 0x2000,
        usb_vid: SupportsIndex = 0x0483,
        mask: Optional[Annotated[numpy.typing.ArrayLike, numpy.bool_]] = None,
    ) -> None:
        super().__init__(serial_number, usb_pid, usb_vid, mask)
        # Skip the upgrade check entirely in non-interactive environments
        # (pipes, CI, Jupyter) — saves the synchronous SN read below.
        try:
            if not sys.stderr.isatty():
                return
        except Exception:
            return

        # The C++ Hand is single-threaded; read identifying values on the
        # construction thread and pass them to the background worker.
        # get_full_system_firmware_version() is a cached accessor (zero
        # SDO I/O), returning 0 when the C++ constructor never populated it.
        try:
            raw_version: int | None = int(self.get_full_system_firmware_version())
            if raw_version == 0:
                raw_version = None
        except Exception:
            raw_version = None

        # SN is reported only when system version >= 1.1.0; gating here
        # avoids the ~3 s timeout that get_product_sn() — still a fresh
        # 6-part SDO read — would incur on legacy firmware.
        sn = ""
        if raw_version is not None:
            major = raw_version & 0xFF
            minor = (raw_version >> 8) & 0xFF
            if (major, minor) >= (1, 1):
                try:
                    sn = self.get_product_sn() or ""
                except Exception:
                    sn = ""

        trigger_check_in_background(sn, raw_version)


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
    __all__ += [
        "TactileGlove",
        "TactileFrame",
        "TactileHandedness",
        "TactileStatus",
        "TactileError",
        "TactileDeviceInfo",
        "TactileFwBuild",
        "TactileDiagnostics",
        "TactileDeviceTime",
        "TactileSyncResult",
        "TACTILE_BOOTLOADER_MAGIC",
    ]
