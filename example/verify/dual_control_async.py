"""
灵巧手异步控制验证

前置条件：
- Python SDK 已安装
- 设备已连接（支持 1-2 只）

用例步骤与预期结果：
1. 连接灵巧手
2. 使用异步方式控制所有手
3. 预期：所有手能并发执行异步控制任务

支持单/双灵巧手：
- 不指定序列号且启用自动扫描时，将扫描并连接所有可用设备
- 通过 --sn 参数显式指定一个或两个序列号以选择要连接的设备
"""

from __future__ import annotations

import asyncio
import math
import sys
from typing import Optional

import numpy as np

import wujihandpy
from _utils import (
    HandInfo,
    connect_hands,
    create_arg_parser,
    print_summary,
)


async def shake(info: HandInfo, phase_offset: float = 0) -> None:
    """
    执行摇摆动作

    Args:
        info: 设备信息
        phase_offset: 相位偏移（用于镜像动作）
    """
    hand = info.hand

    # Enable all joints
    await hand.write_joint_enabled_async(True)
    print(f"  {info.name} 关节已启用")

    # Filtered realtime control
    controller = hand.realtime_controller(
        enable_upstream=False, filter=wujihandpy.filter.LowPass(cutoff_freq=2.0)
    )
    update_rate = 100.0
    update_period = 1.0 / update_rate

    x = 0
    while True:
        y = (1 - math.cos(x + phase_offset)) * 0.8

        controller.set_joint_target_position(
            np.array(
                [
                    [0, 0, 0, 0],  # F1 (拇指)
                    [y, 0, y, y],  # F2 (食指)
                    [y, 0, y, y],  # F3 (中指)
                    [y, 0, y, y],  # F4 (无名指)
                    [y, 0, y, y],  # F5 (小指)
                ],
                dtype=np.float64,
            )
        )

        x += math.pi / update_rate
        await asyncio.sleep(update_period)


async def read_temperature(info: HandInfo) -> None:
    """
    定期读取并打印电机温度

    Args:
        info: 设备信息
    """
    while True:
        temp = await info.hand.read_joint_temperature_async()
        print(f"\n  {info.name} 电机温度 (F2): {temp[1]}")
        await asyncio.sleep(1)


async def run_with_timeout(
    hands: list[HandInfo], duration: float
) -> dict[str, bool]:
    """
    运行带超时的异步控制

    Args:
        hands: 设备列表
        duration: 运行时长（秒），0 表示无限运行

    Returns:
        每只手的验证结果
    """
    results: dict[str, bool] = {info.name: True for info in hands}

    # 创建任务
    tasks = []
    for idx, info in enumerate(hands):
        phase = math.pi * idx  # 不同的手使用不同相位
        tasks.append(asyncio.create_task(shake(info, phase), name=f"shake_{info.name}"))
        tasks.append(asyncio.create_task(read_temperature(info), name=f"temp_{info.name}"))

    try:
        if duration > 0:
            # 等待指定时间后取消所有任务
            await asyncio.sleep(duration)
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
        else:
            # 无限运行直到用户中断
            await asyncio.gather(*tasks)
    except asyncio.CancelledError:
        # 正常的任务取消（如用户中断或外部超时结束），在此静默忽略即可
        pass

    return results


async def async_main(
    serial_numbers: Optional[list[str]] = None, auto_scan: bool = False, duration: float = 5.0
) -> bool:
    """
    异步主函数

    Args:
        serial_numbers: 灵巧手序列号列表
        auto_scan: 是否自动扫描连接设备
        duration: 运行时长（秒）

    Returns:
        是否全部验证通过
    """
    print("=" * 60)
    print("灵巧手异步控制验证")
    print("=" * 60)

    # 连接设备
    print("\n[步骤 1] 连接设备...")
    hands = connect_hands(serial_numbers, auto_scan=auto_scan)
    print(f"  共连接 {len(hands)} 只灵巧手")

    try:
        print(f"\n[步骤 2] 启动异步任务 ({duration}秒)...")
        if duration == 0:
            print("  (按 Ctrl+C 停止)")

        results = await run_with_timeout(hands, duration)

        # 打印总结
        return print_summary(results, "异步控制验证")

    except KeyboardInterrupt:
        print("\n  用户中断")
        return True
    finally:
        # 禁用所有手
        print("\n禁用所有关节...")
        for info in hands:
            try:
                await info.hand.write_joint_enabled_async(False)
                print(f"  {info.name} 关节已禁用")
            except Exception as exc:
                print(f"  无法禁用 {info.name} 关节: {exc}", file=sys.stderr)


def main(serial_numbers: Optional[list[str]] = None, auto_scan: bool = False, duration: float = 5.0) -> bool:
    """
    主函数入口

    Args:
        serial_numbers: 灵巧手序列号列表
        auto_scan: 是否自动扫描连接设备
        duration: 运行时长（秒）

    Returns:
        是否全部验证通过
    """
    try:
        return asyncio.run(async_main(serial_numbers, auto_scan, duration))
    except KeyboardInterrupt:
        print("\n程序已退出")
        return True


if __name__ == "__main__":
    parser = create_arg_parser("灵巧手异步控制验证")
    parser.add_argument(
        "-t",
        "--time",
        type=float,
        default=5.0,
        metavar="SEC",
        help="运行时长（秒），0 表示无限运行（默认: 5）",
    )
    args = parser.parse_args()

    success = main(serial_numbers=args.serial_numbers, auto_scan=args.auto_scan, duration=args.time)
    sys.exit(0 if success else 1)
