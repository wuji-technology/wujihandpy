"""Tactile board example: identity, diagnostics, sample-rate change, streaming.

Requires firmware tactile-wire-protocol v1.0 or newer (see
wh110-firmware-tactile-api/docs/tactile-wire-protocol.md).
"""

import time
import numpy as np
import wujihandpy as w


def main():
    with w.TactileBoard() as tb:
        info = tb.get_device_info()
        build = tb.get_fw_build()
        print(f"Tactile board")
        print(f"  serial:      {info.serial}")
        print(f"  hw_revision: {tuple(info.hw_revision)}")
        print(f"  fw_version:  {tuple(info.fw_version)}  (build {build.git_short_sha})")
        print(f"  handedness:  {tb.get_handedness()}")

        # Slow down a bit so the print loop can keep up.
        tb.set_sample_rate_hz(60)
        print(f"  sample_rate: {tb.get_sample_rate_hz()} Hz")

        tb.set_disconnect_callback(lambda: print("[disconnect] USB lost"))

        seen = 0
        def on_frame(f):
            nonlocal seen
            seen += 1
            valid_mask = ~np.isnan(f.pressure)
            n_valid = int(valid_mask.sum())
            peak = float(f.pressure[valid_mask].max()) if n_valid else float("nan")
            if seen % 30 == 0:
                print(f"  seq={f.sequence:5d}  valid={n_valid}/{24*32}  peak={peak:.3f}")

        tb.start_streaming(on_frame)
        time.sleep(2.0)
        tb.stop_streaming()

        d = tb.get_diagnostics()
        print(
            f"Diagnostics: uptime={d.uptime_ms} ms  frames={d.frame_count}  "
            f"crc_err={d.crc_err_count}  dropouts={d.dropout_count}  "
            f"usb_resets={d.usb_reset_count}"
        )


if __name__ == "__main__":
    main()
