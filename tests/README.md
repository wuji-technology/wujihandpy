# wujihandpy tests

## Unit tests (CI-runnable)

`test_*.py` files use `pytest`. They have no hardware dependency and
run in CI on every PR. Currently this is `test_bridge.py` for the
Zenoh bridge.

## Hardware-in-the-loop tests (local-only, not in this repo)

Anything that talks to a real tactile board needs hardware that isn't
present in CI, so the convention is:

- Name them `tests/hil_*.py`.
- They are excluded from this public repo by `.gitignore`
  (`tests/hil_*.py`) — keep your copy locally.
- Gate the entry with `WUJIHAND_HIL=1` so a stray test runner doesn't
  try to invoke them:

  ```python
  import os, sys
  if os.environ.get("WUJIHAND_HIL") != "1":
      print("hil_xxx.py: skipped (set WUJIHAND_HIL=1 to run)")
      sys.exit(0)
  ```

- Hardware prerequisites for the tactile suite: a tactile board (USB
  VID `0x0483`, PID `0x5700`) flashed with firmware from
  [`wh110-firmware-tactile-api`](https://github.com/wuji-technology/wh110-firmware-tactile-api)
  PR #30 or newer, with TBIM written via the firmware repo's
  `tools/prodtest/flash.py device write` step.

Personal one-offs that you don't even want to share inside the team
should use the `tests/.local_*.py` prefix (also gitignored).
