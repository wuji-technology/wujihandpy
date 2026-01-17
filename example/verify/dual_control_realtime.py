"""
灵巧手实时控制验证

前置条件：
- Python SDK 已安装
- 设备已连接（支持 1-2 只）

用例步骤与预期结果：
1. 连接灵巧手
2. 对每只手进行实时控制测试
3. 预期：所有手都能正常执行实时控制

支持单/双灵巧手：
- 不指定序列号且未开启自动扫描时，默认连接第一个检测到的设备
- 不指定序列号且使用 --auto-scan 参数时，将自动扫描并连接所有检测到的设备
- 通过 --sn 参数指定一个或多个序列号
"""

from __future__ import annotations

import math
import sys
import time
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
    print_summary,
)


def run_realtime_control(hands: list[HandInfo], duration: float = 5.0) -> dict[str, bool]:
    """
    运行实时控制

    Args:
        hands: 设备列表
        duration: 运行时长（秒），0 表示无限运行

    Returns:
        每只手的验证结果
    """
    np.set_printoptions(precision=2, suppress=True)

    # 读取 effort limit
    print("\n[步骤 2] 读取 effort limit...")
    effort_limits: dict[str, np.ndarray] = {}
    effort_supported: dict[str, bool] = {}
    for info in hands:
        effort_limits[info.name] = info.hand.read_joint_effort_limit()
        print(f"  {info.name} effort_limit (F2):\n  {effort_limits[info.name][1]}")

        # 检测是否支持 effort 功能
        print(f"\n  [{info.name}] 检测 effort 功能支持...")
        effort_supported[info.name] = check_effort_support(info.hand, info.name)
        if not effort_supported[info.name]:
            print(f"  [{info.name}] 固件版本 < 1.2.0，不支持 effort 显示")

    # 创建实时控制器
    print("\n[步骤 3] 创建实时控制器...")
    controllers: dict[str, wujihandpy.RealtimeController] = {}
    for info in hands:
        controllers[info.name] = info.hand.realtime_controller(
            enable_upstream=True, filter=wujihandpy.filter.LowPass(cutoff_freq=5.0)
        )
        controllers[info.name].__enter__()
        print(f"  {info.name} 实时控制器已创建")

    results: dict[str, bool] = {info.name: True for info in hands}

    try:
        update_rate = 100.0
        update_period = 1.0 / update_rate

        print(f"\n[步骤 4] 运行实时控制 ({duration}秒)...")
        if duration == 0:
            print("  (按 Ctrl+C 停止)")

        x = 0
        iteration = 0
        start_time = time.time()

        while True:
            # 检查是否超时
            if duration > 0 and (time.time() - start_time) >= duration:
                break

            for idx, info in enumerate(hands):
                # 不同的手使用不同相位（镜像动作）
                phase = math.pi * idx
                y = (1 - math.cos(x + phase)) * 0.8

                target = np.array(
                    [
                        [0, 0, 0, 0],  # F1 (拇指)
                        [y, 0, y, y],  # F2 (食指)
                        [y, 0, y, y],  # F3 (中指)
                        [y, 0, y, y],  # F4 (无名指)
                        [y, 0, y, y],  # F5 (小指)
                    ],
                    dtype=np.float64,
                )

                controller = controllers[info.name]
                controller.set_joint_target_position(target)

                # 每秒打印一次状态
                if iteration % int(update_rate) == 0:
                    error = target - controller.get_joint_actual_position()
                    if effort_supported[info.name]:
                        effort = controller.get_joint_actual_effort()
                        effort_pct = effort / effort_limits[info.name] * 100
                        print(f"  {info.name} - error: {error[1, :]}  effort%: {effort_pct[1, :]}")
                    else:
                        print(f"  {info.name} - error: {error[1, :]}  effort%: N/A (固件不支持)")

            x += math.pi / update_rate
            iteration += 1
            time.sleep(update_period)

    except KeyboardInterrupt:
        print("\n  用户中断")
    except Exception as e:
        print(f"\n  [ERROR] 实时控制异常: {e}")
        for info in hands:
            results[info.name] = False
    finally:
        # 退出实时控制器
        for info in hands:
            try:
                controllers[info.name].__exit__(None, None, None)
            except Exception as e:
                print(f"  [WARN] 退出 {info.name} 的实时控制器时发生异常: {e}")

    return results


def main(serial_numbers: Optional[list[str]] = None, auto_scan: bool = False, duration: float = 5.0) -> bool:
    """
    主函数

    Args:
        serial_numbers: 灵巧手序列号列表
        auto_scan: 是否自动扫描连接设备
        duration: 运行时长（秒）

    Returns:
        是否全部验证通过
    """
    print("=" * 60)
    print("灵巧手实时控制验证")
    print("=" * 60)

    # 连接设备
    print("\n[步骤 1] 连接设备...")
    hands = connect_hands(serial_numbers, auto_scan=auto_scan)
    print(f"  共连接 {len(hands)} 只灵巧手")

    try:
        # 启用所有关节
        enable_all_hands(hands)

        # 运行实时控制
        results = run_realtime_control(hands, duration)

        # 打印总结
        return print_summary(results, "实时控制验证")

    finally:
        # 禁用所有手
        print("\n禁用所有关节...")
        disable_all_hands(hands)


if __name__ == "__main__":
    parser = create_arg_parser("灵巧手实时控制验证")
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
