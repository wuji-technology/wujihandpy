"""穿脱手套 demo: 将所有关节平滑驱至穿戴姿态, 便于穿戴或脱下手套。

拇指对掌内收 (F1J1≈1.1-1.3, F1J2≈0.75 rad) 贴向掌心, 其余四指基本伸直,
形成扁平手型给手套留空间。脚本通过 read_handedness() 自动区分左右手并选择
对应的标定姿态。低通滤波器从当前位姿插值到目标位, 达到稳定时间后自动退出
并失能所有关节。
"""

import time

import numpy as np

import wujihandpy

UPDATE_RATE_HZ = 100.0
SETTLE_TIME_S = 3.0

# 固件 handedness 约定: 1 = 左手, 其他 (通常 0) = 右手
HANDEDNESS_LEFT = 1

# 穿戴手套姿态: 实测时操作员手动摆好后从 read_joint_actual_position() 读取 (单位: rad)
# 每行对应一根手指 F1-F5 (拇指/食指/中指/无名指/小指), 列对应 J1-J4
GLOVE_DONNING_POSE_LEFT = np.array(
    [
        [1.1355, 0.7829, -0.0018, 0.0016],   # F1 拇指: CMC1/CMC2/PIP/DIP
        [0.0012, 0.0977, 0.0001, 0.0026],    # F2 食指
        [-0.0016, 0.0002, -0.0033, 0.0020],  # F3 中指
        [0.0002, -0.1037, 0.0010, 0.0004],   # F4 无名指
        [-0.0022, -0.1559, -0.0006, 0.0029], # F5 小指
    ],
    dtype=np.float64,
)

GLOVE_DONNING_POSE_RIGHT = np.array(
    [
        [1.3029, 0.7528, 0.0190, 0.0082],    # F1 拇指
        [0.0283, -0.0298, 0.0017, 0.0016],   # F2 食指
        [0.0427, 0.0098, 0.0116, 0.0071],    # F3 中指
        [0.0163, 0.0716, 0.0371, 0.0086],    # F4 无名指
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
