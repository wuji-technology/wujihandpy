"""
灵巧手写入控制验证

前置条件：
- Python SDK 已安装
- 设备已连接（支持 1-2 只）

用例步骤与预期结果：
1. 连接灵巧手
2. 对每只手进行写入控制测试
3. 预期：所有手都能正常响应写入命令

支持单/双灵巧手：
- 未指定序列号时，通过自动扫描连接所有可用设备
- 通过 --sn 参数指定一个或多个序列号，仅连接指定设备
"""

from __future__ import annotations

import sys
from typing import Optional

import numpy as np

from _utils import (
    HandInfo,
    connect_hands,
    create_arg_parser,
    disable_all_hands,
    print_summary,
)


def verify_single_hand(hand_info: HandInfo) -> bool:
    """
    对单只手进行写入控制验证

    Args:
        hand_info: 设备信息

    Returns:
        验证是否通过
    """
    hand = hand_info.hand
    hand_name = hand_info.name

    try:
        # Bulk-Write: 启用食指，禁用其他手指
        print(f"\n[{hand_name}] 步骤 1: 启用食指，禁用其他手指...")
        hand.write_joint_enabled(
            np.array(
                [
                    #  J1     J2     J3     J4
                    [False, False, False, False],  # F1
                    [True, True, True, True],  # F2 (食指)
                    [False, False, False, False],  # F3
                    [False, False, False, False],  # F4
                    [False, False, False, False],  # F5
                ],
                dtype=bool,
            )
        )
        print(f"  [{hand_name}] 成功启用食指，禁用其他手指")

        # Bulk-Write: 启用所有关节
        print(f"\n[{hand_name}] 步骤 2: 启用所有关节...")
        hand.write_joint_enabled(True)
        print(f"  [{hand_name}] 成功启用所有关节")

        # 逐个启用所有关节
        print(f"\n[{hand_name}] 步骤 3: 逐个启用所有关节...")
        for i in range(5):
            for j in range(4):
                hand.finger(i).joint(j).write_joint_enabled(True)
        print(f"  [{hand_name}] 成功逐个启用所有关节")

        print(f"\n[{hand_name}] 单手验证结果: PASS")
        return True

    except Exception as e:
        print(f"\n[{hand_name}] [ERROR] 测试过程中发生异常: {e}")
        return False


def main(serial_numbers: Optional[list[str]] = None, auto_scan: bool = False) -> bool:
    """
    主函数

    Args:
        serial_numbers: 灵巧手序列号列表
        auto_scan: 是否自动扫描连接设备

    Returns:
        是否全部验证通过
    """
    print("=" * 60)
    print("灵巧手写入控制验证")
    print("=" * 60)

    # 连接设备
    print("\n[步骤 1] 连接设备...")
    hands = connect_hands(serial_numbers, auto_scan=auto_scan)
    print(f"  共连接 {len(hands)} 只灵巧手")

    try:
        # 对每只手进行验证
        results: dict[str, bool] = {}
        for hand_info in hands:
            print(f"\n{'='*50}")
            print(f"验证: {hand_info.name} (SN: {hand_info.serial_number})")
            print("=" * 50)

            success = verify_single_hand(hand_info)
            results[hand_info.name] = success

        # 打印总结
        all_pass = print_summary(results, "写入控制验证")
        return all_pass

    finally:
        # 禁用所有手
        print("\n禁用所有关节...")
        disable_all_hands(hands)


if __name__ == "__main__":
    parser = create_arg_parser("灵巧手写入控制验证")
    args = parser.parse_args()

    success = main(serial_numbers=args.serial_numbers, auto_scan=args.auto_scan)
    sys.exit(0 if success else 1)
