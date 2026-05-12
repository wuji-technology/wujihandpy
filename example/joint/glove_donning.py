"""穿脱手套 demo: 将所有关节平滑归零（手掌摊平），便于穿戴或脱下手套。

低通滤波器从当前位姿插值到目标 0 位，达到稳定时间后自动退出并失能所有关节。
"""

import time

import numpy as np

import wujihandpy

UPDATE_RATE_HZ = 100.0
SETTLE_TIME_S = 3.0


def main() -> None:
    hand = wujihandpy.Hand()
    try:
        run(hand)
    finally:
        hand.write_joint_enabled(False)


def run(hand: wujihandpy.Hand) -> None:
    hand.write_joint_enabled(True)

    target = np.zeros((5, 4), dtype=np.float64)
    update_period = 1.0 / UPDATE_RATE_HZ

    with hand.realtime_controller(
        enable_upstream=False,
        filter=wujihandpy.filter.LowPass(cutoff_freq=2.0),
    ) as controller:
        deadline = time.monotonic() + SETTLE_TIME_S
        while time.monotonic() < deadline:
            controller.set_joint_target_position(target)
            time.sleep(update_period)

    print("All joints zeroed. Ready for glove donning/doffing.")


if __name__ == "__main__":
    main()
