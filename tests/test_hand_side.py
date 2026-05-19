"""Hand(side=...) construction validation.

The validation/dispatch logic lives in `_resolve_super_init_args`, which
this test exercises directly so no _core.Hand instance is created
(pybind11 forbids monkey-patching __init__ on its C++ types).
"""

from __future__ import annotations

import numpy as np
import pytest

import wujihandpy
from wujihandpy import _core, _resolve_super_init_args


def _resolve(**overrides):
    base = {
        "serial_number": None,
        "side": None,
        "usb_pid": 0x2000,
        "usb_vid": 0x0483,
        "mask": None,
    }
    base.update(overrides)
    return _resolve_super_init_args(**base)


def test_side_and_serial_number_are_mutually_exclusive():
    with pytest.raises(ValueError, match="mutually exclusive"):
        _resolve(side="left", serial_number="WH123")
    # End-to-end check: the error path fires before super().__init__, so the
    # public wrapper raises ValueError without touching libusb.
    with pytest.raises(ValueError, match="mutually exclusive"):
        wujihandpy.Hand(side="left", serial_number="WH123")


@pytest.mark.parametrize("bad", ["l", "LEFT", "Left", "lefty", ""])
def test_invalid_side_literal(bad):
    with pytest.raises(ValueError, match="side must be 'left' or 'right'"):
        _resolve(side=bad)
    with pytest.raises(ValueError, match="side must be 'left' or 'right'"):
        wujihandpy.Hand(side=bad)


def test_side_left_maps_to_enum_left():
    args, kwargs = _resolve(side="left")
    assert args == ()
    assert kwargs["side"] is _core.Hand.Side.Left


def test_side_right_maps_to_enum_right():
    args, kwargs = _resolve(side="right")
    assert args == ()
    assert kwargs["side"] is _core.Hand.Side.Right


def test_serial_number_positional_forwarded_unchanged():
    args, kwargs = _resolve(serial_number="WH123")
    assert kwargs == {}
    assert args[0] == "WH123"


def test_no_arg_routes_to_sn_path():
    args, kwargs = _resolve()
    assert kwargs == {}
    assert args[0] is None


def test_side_with_custom_pid_vid_mask():
    mask = np.array([True, False, True, False] * 5, dtype=bool).reshape(5, 4)
    args, kwargs = _resolve(side="left", usb_pid=0x2001, usb_vid=0x0484, mask=mask)
    assert args == ()
    assert kwargs["side"] is _core.Hand.Side.Left
    assert kwargs["usb_pid"] == 0x2001
    assert kwargs["usb_vid"] == 0x0484
    assert kwargs["mask"] is mask


def test_side_is_keyword_only():
    """Positional Hand('left') must route to serial_number, not side."""
    args, kwargs = _resolve(serial_number="left")
    assert kwargs == {}
    assert args[0] == "left"
    # And the wrapper signature itself rejects positional `side`:
    with pytest.raises(TypeError):
        wujihandpy.Hand("WH123", "left")  # type: ignore[misc]
