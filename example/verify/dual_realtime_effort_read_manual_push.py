"""
实时读取灵巧手 effort 数据（可手动推动）

前置条件：
- 设备已连接

功能：
- 实时显示关节位置和 effort 百分比
- 跟随模式：手可被推动，观察 effort 变化

支持单/双灵巧手：
- 不指定序列号且开启自动扫描时，将扫描并连接所有可用设备
- 通过 --sn 参数指定一个或两个序列号（不使用自动扫描）
"""

from __future__ import annotations

import os
import time
from contextlib import ExitStack
from dataclasses import dataclass
from typing import Optional

import numpy as np
import wujihandpy

from _utils import (
    HandInfo,
    check_effort_support,
    connect_hands,
    create_arg_parser,
    disable_all_hands,
    enable_all_hands,
)


@dataclass
class ControllerContext:
    """控制器上下文"""

    hand_info: HandInfo
    controller: wujihandpy.IController
    effort_limit: np.ndarray
    effort_supported: bool


FINGER_NAMES = ["拇指", "食指", "中指", "无名指", "小指"]


def print_hand_status(
    ctx: ControllerContext,
    show_header: bool = True,
) -> None:
    """
    打印单只手的状态

    Args:
        ctx: 控制器上下文
        show_header: 是否显示表头
    """
    hand_name = ctx.hand_info.name
    controller = ctx.controller
    effort_limit = ctx.effort_limit
    effort_supported = ctx.effort_supported

    # 实时读取关节位置 (5x4 array)
    positions = controller.get_joint_actual_position()

    # 根据是否支持 effort 选择显示内容
    if effort_supported:
        effort = controller.get_joint_actual_effort()
        effort_pct = effort / effort_limit * 100

    # 跟随模式：将当前位置设为目标位置，使手可以被推动
    controller.set_joint_target_position(positions)

    if show_header:
        print(f"\n[{hand_name}] (SN: {ctx.hand_info.serial_number})")
        print("-" * 85)
        print(f"{'手指':<8} {'位置 [J1, J2, J3, J4]':<35} {'Effort % [J1, J2, J3, J4]':<35}")
        print("-" * 85)

    # 按手指分组输出
    for i, name in enumerate(FINGER_NAMES):
        pos = positions[i]
        pos_str = f"[{pos[0]:7.2f}, {pos[1]:7.2f}, {pos[2]:7.2f}, {pos[3]:7.2f}]"
        if effort_supported:
            eff = effort_pct[i]
            eff_str = f"[{eff[0]:6.1f}%, {eff[1]:6.1f}%, {eff[2]:6.1f}%, {eff[3]:6.1f}%]"
        else:
            eff_str = "[  N/A  ,   N/A  ,   N/A  ,   N/A  ]"
        print(f"{name:<8} {pos_str:<35} {eff_str:<35}")


def main(
    freq: float = 100.0,
    serial_numbers: Optional[list[str]] = None,
) -> None:
    """
    主函数

    Args:
        freq: 读取频率 (Hz)
        serial_numbers: 灵巧手序列号列表
    """
    # 连接设备
    print("连接灵巧手设备...")
    hands = connect_hands(serial_numbers)
    print(f"共连接 {len(hands)} 只灵巧手")

    # 读取 effort limit 用于计算百分比
    print("\n读取 Effort Limit...")
    effort_limits = {}
    effort_supported = {}
    for info in hands:
        effort_limits[info.name] = info.hand.read_joint_effort_limit()
        # 检测是否支持 effort 功能
        print(f"  检测 {info.name} effort 功能支持...")
        effort_supported[info.name] = check_effort_support(info.hand, info.name)
        if not effort_supported[info.name]:
            print(f"  {info.name}: 固件版本 < 1.2.0，不支持 effort 显示")

    # 启用所有关节
    print("\n启用所有关节...")
    enable_all_hands(hands)

    interval = 1.0 / freq

    # 清屏
    os.system("clear")

    try:
        # 使用 ExitStack 管理多个控制器上下文
        with ExitStack() as stack:
            contexts: list[ControllerContext] = []

            # 为每只手创建实时控制器
            for info in hands:
                controller = stack.enter_context(
                    info.hand.realtime_controller(
                        enable_upstream=True,
                        filter=wujihandpy.filter.LowPass(cutoff_freq=100.0),
                    )
                )
                contexts.append(
                    ControllerContext(
                        hand_info=info,
                        controller=controller,
                        effort_limit=effort_limits[info.name],
                        effort_supported=effort_supported[info.name],
                    )
                )

            while True:
                start_time = time.perf_counter()

                # 移动光标到起始位置
                print("\033[H", end="")

                # 打印标题
                print(
                    f"实时 Effort 读取 - 频率: {freq}Hz - "
                    f"设备数: {len(hands)} (按 Ctrl+C 停止)"
                )
                print("提示: 跟随模式，推动灵巧手观察 effort 变化")
                print("=" * 85)

                # 打印每只手的状态
                for ctx in contexts:
                    print_hand_status(ctx, show_header=True)

                print("=" * 85)

                # 精确控制读取间隔
                elapsed = time.perf_counter() - start_time
                sleep_time = max(0, interval - elapsed)
                time.sleep(sleep_time)

    except KeyboardInterrupt:
        pass
    finally:
        disable_all_hands(hands)
        print("\n已停止实时读取，关节已禁用")


if __name__ == "__main__":
    parser = create_arg_parser("实时读取灵巧手 effort 数据（可手动推动）")
    parser.add_argument(
        "-f",
        "--freq",
        type=float,
        default=100,
        help="读取频率 (Hz), 默认 100Hz",
    )
    args = parser.parse_args()

    main(freq=args.freq, serial_numbers=args.serial_numbers)
