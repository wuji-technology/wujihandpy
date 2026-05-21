"""Glove donning/doffing demo: smoothly drive all joints to a wear-ready pose.

The thumb is adducted across the palm (F1J1 ~1.1-1.3, F1J2 ~0.75 rad) while
the other four fingers stay nearly extended, leaving room for the glove.
The script uses read_handedness() to pick the matching calibration pose for
the left or right hand. A low-pass filter interpolates from the current
pose to the target, then disables all joints after the settle time elapses.
"""

import time

import numpy as np

import wujihandpy

UPDATE_RATE_HZ = 100.0
SETTLE_TIME_S = 3.0

# Firmware handedness convention: 1 = left hand, others (typically 0) = right hand
HANDEDNESS_LEFT = 1

# Glove donning pose: captured from read_joint_actual_position() after the
# operator manually held the hand in place (unit: rad).
# Rows are fingers F1-F5 (thumb/index/middle/ring/pinky), columns are J1-J4.
GLOVE_DONNING_POSE_LEFT = np.array(
    [
        [1.1355, 0.7829, -0.0018, 0.0016],   # F1 thumb: CMC1/CMC2/PIP/DIP
        [0.0012, 0.0977, 0.0001, 0.0026],    # F2 index
        [-0.0016, 0.0002, -0.0033, 0.0020],  # F3 middle
        [0.0002, -0.1037, 0.0010, 0.0004],   # F4 ring
        [-0.0022, -0.1559, -0.0006, 0.0029], # F5 pinky
    ],
    dtype=np.float64,
)

GLOVE_DONNING_POSE_RIGHT = np.array(
    [
        [1.3029, 0.7528, 0.0190, 0.0082],    # F1 thumb
        [0.0283, -0.0298, 0.0017, 0.0016],   # F2 index
        [0.0427, 0.0098, 0.0116, 0.0071],    # F3 middle
        [0.0163, 0.0716, 0.0371, 0.0086],    # F4 ring
        [-0.0057, 0.1875, 0.0008, -0.0333],  # F5 pinky
    ],
    dtype=np.float64,
)


def main() -> None:
    hand = wujihandpy.Hand()
    try:
        run(hand)
    finally:
        hand.write_joint_enabled(False)


def run(hand: wujihandpy.Hand) -> None:
    if hand.read_handedness() == HANDEDNESS_LEFT:
        target = GLOVE_DONNING_POSE_LEFT.copy()
        side = "left"
    else:
        target = GLOVE_DONNING_POSE_RIGHT.copy()
        side = "right"
    print(f"Detected {side} hand, driving to glove donning pose...")

    hand.write_joint_enabled(True)
    try:
        update_period = 1.0 / UPDATE_RATE_HZ

        with hand.realtime_controller(
            enable_upstream=False,
            filter=wujihandpy.filter.LowPass(cutoff_freq=2.0),
        ) as controller:
            deadline = time.monotonic() + SETTLE_TIME_S
            while time.monotonic() < deadline:
                controller.set_joint_target_position(target)
                time.sleep(update_period)

        print(f"Hand ({side}) in glove donning pose. Ready for glove donning/doffing.")
    finally:
        hand.write_joint_enabled(False)


if __name__ == "__main__":
    main()
