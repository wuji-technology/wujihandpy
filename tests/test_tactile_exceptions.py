"""Tactile exception-type translation invariants — no hardware needed.

Guards against silently regressing the round-1 P1 fix that mapped the
internal C++ exceptions onto stdlib `ConnectionError` / `TimeoutError`.

Platform handling: same as test_tactile_imports.py — non-Linux skips
the whole file at module level; Linux hard-imports so regressions in
the binding fail the test instead of silently skipping.
"""
from __future__ import annotations

import sys

import pytest

if not sys.platform.startswith("linux"):
    pytest.skip(
        "tactile bindings are Linux-only",
        allow_module_level=True,
    )

import wujihandpy.tactile as tactile  # noqa: E402


def _board_not_connected():
    """Construct a Glove against a nonexistent serial; connect()
    returns False and the SDK stays disconnected. Subsequent calls
    must raise ConnectionError, not bare RuntimeError or anything
    else."""
    b = tactile.Glove(serial_number="ci-nonexistent-serial-zzz")
    assert b.connect() is False
    return b


def test_read_frame_on_disconnected_raises_ConnectionError():
    b = _board_not_connected()
    with pytest.raises(ConnectionError):
        b.read_frame(timeout_ms=10)


def test_get_device_info_on_disconnected_raises_ConnectionError():
    b = _board_not_connected()
    with pytest.raises(ConnectionError):
        b.get_device_info()


def test_get_diagnostics_on_disconnected_raises_ConnectionError():
    b = _board_not_connected()
    with pytest.raises(ConnectionError):
        b.get_diagnostics()


def test_set_streaming_on_disconnected_raises_ConnectionError():
    b = _board_not_connected()
    with pytest.raises(ConnectionError):
        b.set_streaming(True)


def test_start_streaming_on_disconnected_raises_ConnectionError():
    b = _board_not_connected()
    with pytest.raises(ConnectionError):
        b.start_streaming(lambda f: None)


def test_enter_context_on_missing_device_raises_ConnectionError():
    """`with tactile.Glove(serial=...)` should raise ConnectionError
    on missing device, not bare RuntimeError. Matches stdlib idioms
    where `socket.connect()` raises ConnectionRefusedError /
    ConnectionError."""
    with pytest.raises(ConnectionError):
        with tactile.Glove(serial_number="ci-nonexistent-serial-zzz"):
            pass


def test_exception_classes_distinct_from_runtime_error():
    """Wire-failure classes should remain stdlib-catchable."""
    b = _board_not_connected()
    try:
        b.read_frame(timeout_ms=5)
    except ConnectionError:
        pass  # expected
    except RuntimeError as e:
        # ConnectionError IS a subclass of OSError IS a subclass of
        # Exception; RuntimeError is a sibling that should never be
        # what we raise from a tactile not-connected path.
        if not isinstance(e, ConnectionError):
            pytest.fail(
                "tactile read_frame on disconnected board raised "
                f"plain RuntimeError ({e!r}); regression of round-1 P1 fix"
            )
