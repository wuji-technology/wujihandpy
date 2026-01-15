"""
realtime_controller 模式验证脚本

前置条件：
- 设备已连接

用例步骤与预期结果：
1. 使用 with hand.realtime_controller(enable_upstream=True) 进入实时模式
2. 调用 get_joint_actual_effort() 获取 effort 数据
3. 预期：正常返回数据，无异常

支持单/双灵巧手：
- 不指定序列号时自动连接第一个设备
- 通过 --sn 参数指定一个或两个序列号
"""

from __future__ import annotations

import sys
from typing import Optional

import numpy as np
import wujihandpy

from _utils import (
    HandInfo,
    check_effort_support,
    connect_hands,
    create_arg_parser,
    print_summary,
)


def verify_single_hand(hand_info: HandInfo) -> bool:
    """
    对单只手进行验证

    Args:
        hand_info: 设备信息

    Returns:
        验证是否通过
    """
    hand = hand_info.hand
    hand_name = hand_info.name

    try:
        # 进入实时模式
        print(f"\n[{hand_name}] 进入实时模式 (enable_upstream=True)...")
        default_filter = wujihandpy.filter.LowPass(10.0)
        with hand.realtime_controller(
            enable_upstream=True, filter=default_filter
        ) as controller:
            print(f"  [{hand_name}] 成功进入实时模式")

            # 检测是否支持 effort 功能
            print(f"\n[{hand_name}] 检测 effort 功能支持...")
            effort_supported = check_effort_support(hand, hand_name)

            if not effort_supported:
                print(f"\n[{hand_name}] [SKIP] 该设备固件版本不支持 effort 功能，跳过验证")
                return True  # 固件不支持不算失败

            # 获取 effort 数据
            print(f"\n[{hand_name}] 调用 get_joint_actual_effort()...")
            effort = controller.get_joint_actual_effort()

            # 验证返回数据
            print(f"\n[{hand_name}] 验证结果")
            print("-" * 40)

            # 检查返回类型
            is_ndarray = isinstance(effort, np.ndarray)
            print(
                f"  [{hand_name}] 返回类型: {type(effort).__name__} "
                f"(预期: ndarray) - {'PASS' if is_ndarray else 'FAIL'}"
            )

            # 检查数据形状 (5x4: 5个手指 x 4个关节)
            expected_shape = (5, 4)
            shape_correct = effort.shape == expected_shape
            print(
                f"  [{hand_name}] 数据形状: {effort.shape} "
                f"(预期: {expected_shape}) - {'PASS' if shape_correct else 'FAIL'}"
            )

            # 检查数据类型
            is_float64 = effort.dtype == np.float64
            print(
                f"  [{hand_name}] 数据类型: {effort.dtype} "
                f"(预期: float64) - {'PASS' if is_float64 else 'FAIL'}"
            )

            # 打印 effort 数据
            np.set_printoptions(precision=4, suppress=True)
            print(f"\n  [{hand_name}] Effort 数据 (5x4):\n{effort}")

            # 多次读取验证稳定性
            print(f"\n[{hand_name}] 连续读取 5 次验证稳定性...")
            for i in range(5):
                effort_i = controller.get_joint_actual_effort()
                print(
                    f"  [{hand_name}] 第 {i + 1} 次读取成功, "
                    f"F2J1 effort: {effort_i[1, 0]:.4f}"
                )

            # 总结
            all_pass = is_ndarray and shape_correct and is_float64
            print(f"\n[{hand_name}] 单手验证结果: {'PASS' if all_pass else 'FAIL'}")

            return all_pass

    except Exception as e:
        print(f"\n[{hand_name}] [ERROR] 验证失败: {e}")
        return False


def main(serial_numbers: Optional[list[str]] = None) -> bool:
    """
    主函数

    Args:
        serial_numbers: 灵巧手序列号列表

    Returns:
        是否全部验证通过
    """
    print("=" * 60)
    print("realtime_controller 模式验证")
    print("=" * 60)

    # 连接设备
    print("\n[步骤 1] 连接设备...")
    hands = connect_hands(serial_numbers)
    print(f"  共连接 {len(hands)} 只灵巧手")

    # 对每只手进行验证
    results: dict[str, bool] = {}
    for hand_info in hands:
        print(f"\n{'='*50}")
        print(f"验证: {hand_info.name} (SN: {hand_info.serial_number})")
        print("=" * 50)

        success = verify_single_hand(hand_info)
        results[hand_info.name] = success

    # 打印总结
    all_pass = print_summary(results, "realtime_controller 验证")
    return all_pass


if __name__ == "__main__":
    parser = create_arg_parser("realtime_controller 模式验证")
    args = parser.parse_args()

    success = main(serial_numbers=args.serial_numbers)
    sys.exit(0 if success else 1)
