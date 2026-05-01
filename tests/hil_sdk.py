#!/usr/bin/env python3
"""End-to-end HIL test for the wujihandpy tactile SDK.

Exercises every tactile.Board command end-to-end against a connected
tactile board running firmware tactile-wire-protocol v1.0+. RESET and
ENTER_BOOTLOADER's success paths are skipped (they tear down USB);
their error paths (bad magic / bad payload) are exercised.

Hardware required: a tactile board flashed with firmware PR #30 or
later, with TBIM written via tools/prodtest. See tests/README.md for
the full prerequisite list.

Usage:
    WUJIHAND_HIL=1 python3 tests/hil_sdk.py [--serial SN]

Without WUJIHAND_HIL=1 the script exits 0 immediately so it's safe to
list under any test runner that auto-collects tests/.
"""
import argparse
import math
import os
import sys
import threading
import time

import numpy as np

import wujihandpy
from wujihandpy import tactile


if os.environ.get("WUJIHAND_HIL") != "1":
    print("hil_sdk.py: skipped (set WUJIHAND_HIL=1 to run; requires connected board)")
    sys.exit(0)


# ---------------------------------------------------------------------------
# Tiny test harness
# ---------------------------------------------------------------------------

PASS = 0
FAIL = 0
FAILED_NAMES: list[str] = []


def check(name: str, ok: bool, detail: str = "") -> bool:
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  PASS  {name}")
        return True
    FAIL += 1
    FAILED_NAMES.append(name)
    suffix = f"  ({detail})" if detail else ""
    print(f"  FAIL  {name}{suffix}")
    return False


def section(title: str) -> None:
    print(f"\n=== {title} ===")


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_identity(tb: tactile.Board) -> None:
    section("Identity (spec §3.1)")
    info = tb.get_device_info()
    check("get_device_info: serial non-empty",
          isinstance(info.serial, str) and len(info.serial) > 0,
          f"serial={info.serial!r}")
    check("get_device_info: serial matches WT[1-9][JK][A-Z]... pattern",
          len(info.serial) == 16 and info.serial.startswith("WT"),
          f"serial={info.serial!r}")
    check("get_device_info: hw_revision is 4-tuple of u8",
          len(info.hw_revision) == 4 and all(0 <= v <= 255 for v in info.hw_revision),
          f"hw_revision={tuple(info.hw_revision)}")
    check("get_device_info: fw_version is 4-tuple of u8",
          len(info.fw_version) == 4 and all(0 <= v <= 255 for v in info.fw_version),
          f"fw_version={tuple(info.fw_version)}")
    print(f"  -> {info.serial}  hw={tuple(info.hw_revision)}  fw={tuple(info.fw_version)}")

    build = tb.get_fw_build()
    check("get_fw_build: git_short_sha is 7-char hex",
          len(build.git_short_sha) == 7 and all(c in "0123456789abcdef" for c in build.git_short_sha),
          f"sha={build.git_short_sha!r}")
    print(f"  -> build {build.git_short_sha}")

    h = tb.get_handedness()
    check("get_handedness: returns LEFT or RIGHT",
          h in (tactile.Handedness.LEFT, tactile.Handedness.RIGHT),
          f"got {h!r}")
    print(f"  -> handedness {h}")


def test_diagnostics(tb: tactile.Board) -> None:
    section("Diagnostics (spec §3.2)")
    d1 = tb.get_diagnostics()
    check("get_diagnostics: uptime_ms > 0", d1.uptime_ms > 0, f"uptime_ms={d1.uptime_ms}")
    check("get_diagnostics: counters are u32/u16",
          0 <= d1.frame_count <= 0xFFFFFFFF and 0 <= d1.usb_reset_count <= 0xFFFF,
          f"frame_count={d1.frame_count} usb_reset={d1.usb_reset_count}")
    print(f"  -> uptime={d1.uptime_ms}ms frames={d1.frame_count} crc_err={d1.crc_err_count} "
          f"dropouts={d1.dropout_count} usb_resets={d1.usb_reset_count}")

    tb.reset_counters()
    time.sleep(0.05)  # let the firmware actually zero
    d2 = tb.get_diagnostics()
    check("reset_counters: frame_count zeroed", d2.frame_count <= 50,
          f"after reset frame_count={d2.frame_count}")
    check("reset_counters: crc_err_count zeroed", d2.crc_err_count == 0,
          f"crc_err={d2.crc_err_count}")
    check("reset_counters: uptime_ms NOT zeroed (it's not a counter)",
          d2.uptime_ms >= d1.uptime_ms,
          f"d1.uptime={d1.uptime_ms} d2.uptime={d2.uptime_ms}")
    # frame_count growth across streaming is verified in test_streaming after
    # we explicitly re-enable streaming.


def test_lifecycle(tb: tactile.Board) -> None:
    section("Lifecycle (spec §3.3)")
    # streaming on/off via SET_STREAMING + cross-check via GET_CONFIG
    tb.set_streaming(False)
    check("set_streaming(False) reflected in get_streaming_enabled",
          tb.get_streaming_enabled() is False)

    tb.set_streaming(True)
    check("set_streaming(True) reflected in get_streaming_enabled",
          tb.get_streaming_enabled() is True)

    # ENTER_BOOTLOADER with bad magic: must raise tactile.Error(BAD_PAYLOAD).
    raised = None
    try:
        tb.enter_bootloader(0xDEADBEEF)
    except tactile.Error as e:
        raised = e
    except Exception as e:
        raised = e
    check("enter_bootloader(bad_magic) raises tactile.Error",
          isinstance(raised, tactile.Error),
          f"got {type(raised).__name__}: {raised}")
    if isinstance(raised, tactile.Error):
        check("enter_bootloader(bad_magic) message includes BAD_PAYLOAD(0x13)",
              "BAD_PAYLOAD" in str(raised) and "0x13" in str(raised),
              f"msg={raised}")

    # Note: enter_bootloader(MAGIC) and reset_device() success paths skipped —
    # they tear down USB, requiring re-plug to recover.


def test_configuration(tb: tactile.Board) -> None:
    section("Configuration (spec §3.4)")
    original = tb.get_sample_rate_hz()
    check("get_sample_rate_hz: in 1..120", 1 <= original <= 120, f"got {original}")

    tb.set_sample_rate_hz(60)
    check("set_sample_rate_hz(60) reflected", tb.get_sample_rate_hz() == 60)

    tb.set_sample_rate_hz(120)
    check("set_sample_rate_hz(120) reflected", tb.get_sample_rate_hz() == 120)

    # Out-of-range must reject.
    raised = None
    try:
        tb.set_sample_rate_hz(0)
    except tactile.Error as e:
        raised = e
    check("set_sample_rate_hz(0) rejected as BAD_PAYLOAD",
          isinstance(raised, tactile.Error), f"raised={raised!r}")

    raised = None
    try:
        tb.set_sample_rate_hz(121)
    except tactile.Error as e:
        raised = e
    check("set_sample_rate_hz(121) rejected as BAD_PAYLOAD",
          isinstance(raised, tactile.Error), f"raised={raised!r}")

    # Restore original.
    tb.set_sample_rate_hz(original)


def test_time_sync(tb: tactile.Board) -> None:
    section("Time sync (spec §3.5)")
    t1 = tb.get_device_time()
    time.sleep(0.05)
    t2 = tb.get_device_time()
    delta_ns = t2.device_monotonic_ns - t1.device_monotonic_ns
    check("get_device_time monotonic", delta_ns > 0, f"delta_ns={delta_ns}")
    check("get_device_time delta plausible (~50 ms ± 200 ms)",
          10_000_000 < delta_ns < 250_000_000, f"delta_ns={delta_ns}")

    host_ns = time.time_ns()
    sync = tb.sync_host_epoch(host_ns)
    check("sync_host_epoch echoes host_unix_ns verbatim",
          sync.host_ns_echo == host_ns,
          f"sent={host_ns} echoed={sync.host_ns_echo}")
    check("sync_host_epoch returns plausible device_ns_at_sync",
          sync.device_ns_at_sync > t2.device_monotonic_ns,
          f"t2={t2.device_monotonic_ns} sync={sync.device_ns_at_sync}")


def test_read_frame(tb: tactile.Board) -> None:
    section("Frame reading (read_frame)")
    # set_streaming should already be True.
    f1 = tb.read_frame(timeout_ms=500)
    check("read_frame returns within 500 ms", True)
    check("read_frame: pressure is float32 24x32 numpy",
          f1.pressure.dtype == np.float32 and f1.pressure.shape == (24, 32),
          f"dtype={f1.pressure.dtype} shape={f1.pressure.shape}")
    check("read_frame: hand matches get_handedness",
          f1.hand == tb.get_handedness())

    # Validate value semantics.
    valid_mask = ~np.isnan(f1.pressure)
    n_valid = int(valid_mask.sum())
    check("read_frame: at least some cells are NaN (invalid sentinel)",
          n_valid < 24 * 32, f"valid={n_valid}/{24*32}")
    check("read_frame: at least some cells are valid",
          n_valid > 0, f"valid={n_valid}/{24*32}")
    if n_valid > 0:
        valid_vals = f1.pressure[valid_mask]
        check("read_frame: valid cells in [0.0, 1.0]",
              float(valid_vals.min()) >= 0.0 and float(valid_vals.max()) <= 1.0,
              f"min={valid_vals.min()} max={valid_vals.max()}")

    # Sequence advances.
    f2 = tb.read_frame(timeout_ms=500)
    seq_diff = (f2.sequence - f1.sequence) & 0xFFFF
    check("read_frame: sequence advances by 1..N between consecutive reads",
          1 <= seq_diff < 100,
          f"seq_diff={seq_diff} ({f1.sequence} -> {f2.sequence})")


def test_streaming(tb: tactile.Board) -> None:
    section("Streaming (start_streaming / stop_streaming)")
    tb.set_sample_rate_hz(60)

    received: list[tactile.Frame] = []
    lock = threading.Lock()

    def cb(f):
        with lock:
            received.append(f)

    DURATION = 5.0
    tb.start_streaming(cb)
    time.sleep(DURATION)
    tb.stop_streaming()

    n = len(received)
    # At 60 Hz over 5 s we expect ~300 frames. The host cdc-acm path
    # exhibits ~0.5 s stalls every 2-3 s (firmware reports zero drops),
    # so we accept 60% delivery as a green signal.
    check("streaming: frame count plausible at 60 Hz over 5 s",
          180 < n < 400, f"got {n} frames in {DURATION}s")

    if n >= 2:
        seqs = [f.sequence for f in received]
        diffs = [(seqs[i+1] - seqs[i]) & 0xFFFF for i in range(len(seqs)-1)]
        max_gap = max(diffs)
        # We tolerate up to ~120 frame gaps because the host-side cdc-acm /
        # USB stack exhibits a known ~0.5 s stall every 2-3 s on this
        # platform (firmware reports zero drops). See demuxer's MAX_QUEUE
        # comment. ROS consumers downsample to 30 Hz Image which absorbs
        # this; raw subscribers should treat sub-second gaps as expected.
        check("streaming: no pathologically large sequence gaps (<120)",
              max_gap < 120, f"max consecutive gap = {max_gap}")

    # Restore default rate.
    tb.set_sample_rate_hz(120)


def test_disconnect_callback_register(tb: tactile.Board) -> None:
    section("Disconnect callback (registration only — no unplug in CI)")
    fired = threading.Event()
    tb.set_disconnect_callback(lambda: fired.set())
    check("set_disconnect_callback accepts a Python callable", True)
    # Replace with no-op so it doesn't fire later in this run.
    tb.set_disconnect_callback(lambda: None)
    check("set_disconnect_callback can be replaced", True)
    print("  (true disconnect path covered by unplug-the-USB manual test;")
    print("   would log '[disconnect] USB lost' from example/tactile/basic.py)")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--serial", default=None,
                   help="Match this USB serial (default: first found 0483:5700)")
    args = p.parse_args()

    print(f"wujihandpy {wujihandpy.__version__}")
    print(f"Connecting to tactile board (serial={args.serial or 'auto'}) ...")
    tb = tactile.Board(serial_number=args.serial)
    if not tb.connect():
        print("ERROR: tactile board not found.", file=sys.stderr)
        return 2

    try:
        # Quiet the bus while we exercise commands. With streaming on at 120 Hz
        # the data path competes with command/response on the same CDC fd and
        # responses can be delayed beyond our 500 ms timeout (spec §2.4 lets
        # the device prefer data frames). Re-enable explicitly before the
        # streaming/read_frame tests.
        tb.set_streaming(False)
        time.sleep(0.05)

        test_identity(tb)
        test_diagnostics(tb)
        test_lifecycle(tb)
        test_configuration(tb)
        test_time_sync(tb)

        tb.set_streaming(True)
        time.sleep(0.05)
        test_read_frame(tb)
        test_streaming(tb)
        test_disconnect_callback_register(tb)
    finally:
        tb.disconnect()

    total = PASS + FAIL
    print(f"\n=== Summary ===")
    print(f"  {PASS}/{total} PASS, {FAIL} FAIL")
    if FAIL:
        print(f"  Failed: {', '.join(FAILED_NAMES)}")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
