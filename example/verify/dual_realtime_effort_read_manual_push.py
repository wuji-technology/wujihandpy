"""
实时读取灵巧手 effort 数据（可手动推动）

前置条件：
- 设备已连接

功能：
- 实时显示关节位置和 effort 百分比
- 跟随模式：手可被推动，观察 effort 变化
- 支持滚动模式（默认，适合日志）和覆盖模式（--overlay）

支持单/双灵巧手：
- 不指定序列号且开启自动扫描时，将扫描并连接所有可用设备
- 通过 --sn 参数指定一个或两个序列号（不使用自动扫描）
"""

from __future__ import annotations

import os
import time
from contextlib import ExitStack
from dataclasses import dataclass
from datetime import datetime
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
    scroll_mode: bool = False,
    timestamp: Optional[str] = None,
) -> None:
    """
    打印单只手的状态

    Args:
        ctx: 控制器上下文
        show_header: 是否显示表头
        scroll_mode: 是否为滚动模式
        timestamp: 时间戳（滚动模式使用）
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

    if scroll_mode:
        # 滚动模式：紧凑单行输出，带时间戳
        ts = timestamp or datetime.now().strftime("%H:%M:%S.%f")[:-3]
        for i, name in enumerate(FINGER_NAMES):
            pos = positions[i]
            if effort_supported:
                eff = effort_pct[i]
                print(
                    f"[{ts}] {hand_name} {name}: "
                    f"pos=[{pos[0]:6.1f},{pos[1]:6.1f},{pos[2]:6.1f},{pos[3]:6.1f}] "
                    f"eff%=[{eff[0]:5.1f},{eff[1]:5.1f},{eff[2]:5.1f},{eff[3]:5.1f}]"
                )
            else:
                print(
                    f"[{ts}] {hand_name} {name}: "
                    f"pos=[{pos[0]:6.1f},{pos[1]:6.1f},{pos[2]:6.1f},{pos[3]:6.1f}] "
                    f"eff%=[N/A]"
                )
    else:
        # 覆盖模式：表格式输出
        if show_header:
            print(f"\n[{hand_name}] (SN: {ctx.hand_info.serial_number})")
            print("-" * 85)
            print(
                f"{'手指':<8} {'位置 [J1, J2, J3, J4]':<35} "
                f"{'Effort % [J1, J2, J3, J4]':<35}"
            )
            print("-" * 85)

        # 按手指分组输出
        for i, name in enumerate(FINGER_NAMES):
            pos = positions[i]
            pos_str = f"[{pos[0]:7.2f}, {pos[1]:7.2f}, {pos[2]:7.2f}, {pos[3]:7.2f}]"
            if effort_supported:
                eff = effort_pct[i]
                eff_str = (
                    f"[{eff[0]:6.1f}%, {eff[1]:6.1f}%, {eff[2]:6.1f}%, {eff[3]:6.1f}%]"
                )
            else:
                eff_str = "[  N/A  ,   N/A  ,   N/A  ,   N/A  ]"
            print(f"{name:<8} {pos_str:<35} {eff_str:<35}")


def main(
    freq: float = 100.0,
    serial_numbers: Optional[list[str]] = None,
    overlay_mode: bool = False,
    print_interval: float = 0.1,
) -> None:
    """
    主函数

    Args:
        freq: 控制/读取频率 (Hz)
        serial_numbers: 灵巧手序列号列表
        overlay_mode: 是否使用覆盖模式（实时刷新屏幕）
        print_interval: 滚动模式下的打印间隔 (秒)
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

    # 仅覆盖模式清屏
    if overlay_mode:
        os.system("clear")

    # 滚动模式下的打印信息
    if not overlay_mode:
        print(
            f"\n滚动模式 - 控制频率: {freq}Hz, 打印间隔: {print_interval}s "
            f"(按 Ctrl+C 停止)"
        )
        print("提示: 跟随模式，推动灵巧手观察 effort 变化")
        print("=" * 100)

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

            last_print_time = 0.0
            while True:
                start_time = time.perf_counter()

                if overlay_mode:
                    # 覆盖模式：每次刷新全部内容
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
                        print_hand_status(ctx, show_header=True, scroll_mode=False)

                    print("=" * 85)
                else:
                    # 滚动模式：实时控制，定期打印
                    # 先执行控制（跟随模式）
                    for ctx in contexts:
                        positions = ctx.controller.get_joint_actual_position()
                        ctx.controller.set_joint_target_position(positions)

                    # 按打印间隔输出日志
                    if start_time - last_print_time >= print_interval:
                        timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                        for ctx in contexts:
                            print_hand_status(
                                ctx,
                                show_header=False,
                                scroll_mode=True,
                                timestamp=timestamp,
                            )
                        print()  # 每批数据后空一行
                        last_print_time = start_time

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
        help="控制/读取频率 (Hz), 默认 100Hz",
    )
    parser.add_argument(
        "--overlay",
        action="store_true",
        help="启用覆盖模式，实时刷新屏幕（默认为滚动模式，适合日志记录）",
    )
    parser.add_argument(
        "--print-interval",
        type=float,
        default=0.1,
        help="滚动模式下的打印间隔 (秒), 默认 0.1s",
    )
    args = parser.parse_args()

    main(
        freq=args.freq,
        serial_numbers=args.serial_numbers,
        overlay_mode=args.overlay,
        print_interval=args.print_interval,
    )
