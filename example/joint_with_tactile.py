"""Drive joint motion while reading tactile pressure in the same process.

Hand and TactileGlove use independent USB transports (libusb async vs CDC
ACM) and independent threads, so the two APIs compose without coordination.
The frame callback fires on the glove's streaming consumer thread; the
joint commands run on whichever thread you call them from.

Hardware:
  - Joint controller (PID 0x2000) on the bus
  - Tactile sensing glove (PID 0x5700) on the bus

When both PIDs share VID 0x0483, `Hand()` is pinned to PID 0x2000 by
default; pass `serial_number=` if you have more than one of the same
device on the bus.
"""
from __future__ import annotations

import math
import time

import numpy as np

import wujihandpy


def main() -> None:
    hand = wujihandpy.Hand()
    glove = wujihandpy.TactileGlove()

    if not glove.connect():
        raise RuntimeError("Glove not found on the bus (PID 0x5700)")

    # Hold the most recent frame for the joint loop to read.
    latest_pressure_max = 0.0

    def on_frame(frame: wujihandpy.TactileFrame) -> None:
        nonlocal latest_pressure_max
        # NaN cells mark invalid taxels — ignore them.
        valid = frame.pressure[~np.isnan(frame.pressure)]
        latest_pressure_max = float(valid.max()) if valid.size else 0.0

    glove.set_streaming(True)
    glove.start_streaming(on_frame)

    hand.write_joint_enabled(True)
    try:
        with hand.realtime_controller(
            enable_upstream=True,
            filter=wujihandpy.filter.LowPass(cutoff_freq=5.0),
        ) as controller:
            print("Squeeze the glove to dampen the motion. Ctrl-C to exit.")
            update_period = 0.01  # 100 Hz
            x = 0.0
            t_next = time.perf_counter()
            while True:
                # Tactile feedback scales motion amplitude: relaxed glove
                # → full sweep; pressed glove → smaller sweep.
                amplitude = 0.4 * max(0.0, 1.0 - latest_pressure_max)
                y = (1.0 - math.cos(x)) * amplitude

                target = np.array(
                    [
                        [0, 0, 0, 0],  # F1 thumb
                        [y, 0, y, y],  # F2
                        [y, 0, y, y],  # F3
                        [y, 0, y, y],  # F4
                        [y, 0, y, y],  # F5
                    ],
                    dtype=np.float64,
                )
                controller.set_joint_target_position(target)

                x += math.pi / 100.0
                t_next += update_period
                slack = t_next - time.perf_counter()
                if slack > 0:
                    time.sleep(slack)
    except KeyboardInterrupt:
        pass
    finally:
        hand.write_joint_enabled(False)
        glove.stop_streaming()
        glove.disconnect()


if __name__ == "__main__":
    main()
