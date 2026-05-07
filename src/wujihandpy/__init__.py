from __future__ import annotations

import sys
from typing import TYPE_CHECKING, Annotated, SupportsInt

from . import _core
# `filter` and `logging` are wujihandpy submodules; the same-name shadowing
# of Python builtins is intentional and part of the public API surface.
from ._core import Finger, Joint, IController, filter, logging  # noqa: F401, A004
from ._upgrade_check import trigger_check_in_background
from ._version import __version__

# Tactile bindings are Linux-only; keep the rest of wujihandpy importable
# elsewhere. The except is narrow on purpose: only "the submodule itself
# isn't shipped on this platform" should downgrade silently. Anything else
# (ABI mismatch, missing transitive dep, syntax error in tactile.py, an
# AttributeError raised inside the import) must propagate so users can
# diagnose the real failure instead of silently losing the API surface.
try:
    from . import tactile  # type: ignore[attr-defined]
except ModuleNotFoundError as _tactile_err:
    if _tactile_err.name != f"{__name__}.tactile":
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
        serial_number: str | None = None,
        usb_pid: SupportsInt = 0x2000,
        usb_vid: SupportsInt = 0x0483,
        mask: Annotated[numpy.typing.ArrayLike, numpy.bool_] | None = None,
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
    __all__.append("tactile")
