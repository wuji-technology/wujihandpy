# wujihandpy tests

Two layers of tests live here.

## Unit tests (CI-runnable)

`test_*.py` files use `pytest`. They have no hardware dependency and
run in CI on every PR. Currently this is `test_bridge.py` for the
Zenoh bridge.

## Hardware-in-the-loop tests (manual, gated)

`hil_*.py` files exercise the full SDK against a connected tactile
board. They are committed to the repo so reviewers and downstream
consumers can reproduce results, but they are **not** run by `pytest`
or by CI. Each script self-skips when the env var `WUJIHAND_HIL=1`
is not set:

```bash
$ python3 tests/hil_sdk.py
hil_sdk.py: skipped (set WUJIHAND_HIL=1 to run; requires connected board)

$ WUJIHAND_HIL=1 python3 tests/hil_sdk.py
=== Identity (spec §3.1) ===
  PASS  get_device_info: serial non-empty
  ...
```

### `hil_sdk.py`

End-to-end coverage of every command on `wujihandpy.tactile.Board`
(spec §3.1 – §3.5) plus streaming and disconnect-callback registration.
Around 35 individual checks; runs in ~15 s.

**Hardware prerequisites**:

- Tactile board (USB VID `0x0483`, PID `0x5700`) connected.
- Board flashed with firmware from
  [`wh110-firmware-tactile-api` PR #30](https://github.com/wuji-technology/wh110-firmware-tactile-api/pull/30)
  or any later commit that ships the wire-protocol v1 changes.
- TBIM (factory metadata at flash sector 7) written via
  `wh110-firmware-tactile-api/tools/prodtest/flash.py device write`.
  Without TBIM, `get_device_info()` / `get_handedness()` will fail.

**Commands the script touches that have side effects**:

- `reset_counters` — zeroes diagnostics counters on the device.
- `set_streaming(False)` then `set_streaming(True)` — leaves streaming
  ON at exit (the script's last action).
- `set_sample_rate_hz(60)` then `(120)` then back to original — leaves
  the device at the rate it found.
- `enter_bootloader(0xDEADBEEF)` — uses a bad magic; the device
  rejects with `BAD_PAYLOAD(0x13)` and stays running. Does NOT jump
  to bootloader.

It does **not** call `reset_device()` or `enter_bootloader(MAGIC)` —
those tear down USB and require a re-plug to recover.

### Adding new HIL files

Match the `hil_*.py` prefix and gate with the same
`WUJIHAND_HIL=1` check at the top:

```python
import os, sys
if os.environ.get("WUJIHAND_HIL") != "1":
    print("hil_xxx.py: skipped (set WUJIHAND_HIL=1 to run)")
    sys.exit(0)
```

### Personal one-offs

The `.gitignore` rule `tests/.local_*.py` keeps personal experiment
scripts out of git. Use that prefix for anything you don't intend to
commit.
