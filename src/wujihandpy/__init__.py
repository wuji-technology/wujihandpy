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
        # The C++ Hand binds operations to the construction thread, so we
        # read SN + firmware version here on the main thread and hand the
        # pure values off to a background worker.
        #
        # The C++ constructor (Hand::check_firmware_version) already reads
        # FullSystemFirmwareVersion when the main board version is high
        # enough; the cached value is exposed via the no-I/O accessor
        # `get_full_system_firmware_version()`. Returns 0 when the C++
        # layer never populated it (very old hardware).
        try:
            raw_version: int | None = int(self.get_full_system_firmware_version())
            if raw_version == 0:
                raw_version = None
        except Exception:
            raw_version = None

        # SN reporting requires system version >= 1.1.0 (matches the gating
        # in wujihandcpp/include/wujihandcpp/device/hand.hpp:115).
        # NOTE: get_product_sn() is currently NOT a cached getter — it
        # performs 6 fresh SDO reads (see wrapper.hpp:329). Until C++
        # exposes a cached variant this re-reads values that the C++
        # constructor already fetched. Acceptable on modern firmware
        # (~300 ms, gated by version check) but worth a future fix.
        sn = ""
        if raw_version is not None:
            byte_major = raw_version & 0xFF
            byte_minor = (raw_version >> 8) & 0xFF
            if (byte_major, byte_minor) >= (1, 1):
                try:
                    sn = self.get_product_sn() or ""
                except Exception:
                    sn = ""

        trigger_check_in_background(sn, raw_version)


__all__ = ["__version__", "Hand", "Finger", "Joint", "IController", "filter"]
