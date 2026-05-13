"""穿脱手套 demo: 将所有关节平滑驱至穿戴姿态, 便于穿戴或脱下手套。

拇指对掌内收 (F1J1≈1.30, F1J2≈0.75 rad) 贴向掌心, 其余四指基本伸直,
形成扁平手型给手套留空间。低通滤波器从当前位姿插值到目标位,
达到稳定时间后自动退出并失能所有关节。
"""

import time

import numpy as np

import wujihandpy

UPDATE_RATE_HZ = 100.0
SETTLE_TIME_S = 3.0

# 穿戴手套姿态: 实测时手动摆好后从 read_joint_actual_position() 读取得到 (单位: rad)
GLOVE_DONNING_POSE = np.array(
    [
        [1.3029, 0.7528, 0.0190, 0.0082],  # F1 拇指: CMC1/CMC2/PIP/DIP
        [0.0283, -0.0298, 0.0017, 0.0016],  # F2 食指
        [0.0427, 0.0098, 0.0116, 0.0071],  # F3 中指
        [0.0163, 0.0716, 0.0371, 0.0086],  # F4 无名指
        [-0.0057, 0.1875, 0.0008, -0.0333],  # F5 小指
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
    hand.write_joint_enabled(True)
    try:
        target = GLOVE_DONNING_POSE.copy()
        update_period = 1.0 / UPDATE_RATE_HZ

        with hand.realtime_controller(
            enable_upstream=False,
            filter=wujihandpy.filter.LowPass(cutoff_freq=2.0),
        ) as controller:
            deadline = time.monotonic() + SETTLE_TIME_S
            while time.monotonic() < deadline:
                controller.set_joint_target_position(target)
                time.sleep(update_period)

        print("Hand in glove donning pose. Ready for glove donning/doffing.")
    finally:
        hand.write_joint_enabled(False)


if __name__ == "__main__":
    main()
