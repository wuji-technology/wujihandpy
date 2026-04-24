from __future__ import annotations

from . import _core
from ._core import Finger, Joint, IController, filter, logging  # noqa: F401
from ._upgrade_check import trigger_check_in_background
from ._version import __version__


class Hand(_core.Hand):
    """Hand with automatic background firmware-upgrade check.

    The check runs in a daemon thread after __init__ returns; any failure
    is silently absorbed so Hand() behaves identically to the underlying
    C++ binding.
    """

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
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


__all__ = ["__version__", "Hand", "Finger", "Joint", "IController", "filter"]
