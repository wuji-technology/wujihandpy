from __future__ import annotations

from . import _core
from ._core import Finger, Joint, IController, filter, logging  # noqa: F401
from ._upgrade_check import trigger_check_in_background
from ._version import __version__


class Hand(_core.Hand):
    """wujihandpy Hand with automatic background firmware-upgrade check.

    The check runs in a daemon thread after __init__ returns. It is a
    pure add-on: if it fails for any reason (no network, old firmware,
    non-TTY stderr, ...) the user-visible behavior of Hand() is identical
    to the pure C++ binding.
    """

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        # The C++ Hand binds operations to the construction thread for
        # thread-safety. Read SN + firmware version here on the main
        # thread, then hand the pure values off to a background worker.
        #
        # IMPORTANT: get_product_sn() is NOT cached — it does a 6-part SDO
        # read that times out (~3 s) on firmware older than system v1.1.0.
        # We mirror the C++ gating logic (hand.hpp:check_firmware_version)
        # to avoid the timeout on legacy devices: read system version
        # first, only attempt SN read when the version says SN is supported.
        raw_version: int | None = None
        try:
            raw_version = int(self.read_full_system_firmware_version(timeout=0.5))
        except Exception:
            pass

        sn = ""
        if raw_version is not None:
            byte_major = raw_version & 0xFF
            byte_minor = (raw_version >> 8) & 0xFF
            # SN reporting requires system version >= 1.1.0
            if (byte_major, byte_minor) >= (1, 1):
                try:
                    sn = self.get_product_sn() or ""
                except Exception:
                    sn = ""

        trigger_check_in_background(sn, raw_version)


__all__ = ["__version__", "Hand", "Finger", "Joint", "IController", "filter"]
