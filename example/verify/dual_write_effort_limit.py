"""
write_joint_effort_limit 功能验证脚本

前置条件：
- Python SDK 已安装
- 设备已连接

用例步骤与预期结果：
1. 调用 write_joint_effort_limit() 方法设置限制值
2. 预期：方法正常执行，设置生效

支持单/双灵巧手：
- 不指定序列号时自动连接第一个设备
- 通过 --sn 参数指定一个或两个序列号
"""

from __future__ import annotations

import sys
import time
from typing import Optional

import numpy as np

from _utils import (
    HandInfo,
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
    is_success = False
    original_limits = None

    try:
        # 1. 读取当前 Effort Limit (备份)
        print(f"\n[{hand_name}] 读取当前 Effort Limit (作为备份)...")
        original_limits = hand.read_joint_effort_limit()
        print(f"  [{hand_name}] 当前 Effort Limit:\n{original_limits}")

        # 2. 设置新的 Effort Limit
        test_limit_value = 1.0
        test_limits = np.full((5, 4), test_limit_value, dtype=np.float64)

        print(
            f"\n[{hand_name}] 设置新的 Effort Limit 为全是 {test_limit_value} A ..."
        )
        hand.write_joint_effort_limit(test_limits)
        print(f"  [{hand_name}] 写入命令执行完成")

        time.sleep(0.1)

        # 3. 再次读取并验证
        print(f"\n[{hand_name}] 读取并验证设置是否生效...")
        current_limits = hand.read_joint_effort_limit()
        print(f"  [{hand_name}] 读取到的 Effort Limit:\n{current_limits}")

        # 验证差异
        diff = np.abs(current_limits - test_limits)
        max_diff = np.max(diff)
        print(f"  [{hand_name}] 最大误差: {max_diff:.6f} A")

        is_success = max_diff < 0.002
        if is_success:
            print(f"  [{hand_name}] [PASS] 设置生效 (误差在允许范围内)")
        else:
            print(
                f"  [{hand_name}] [FAIL] 设置未生效或误差过大 "
                f"(期望: {test_limit_value}, 实际最大偏差: {max_diff})"
            )

    except Exception as e:
        print(f"\n[{hand_name}] [ERROR] 测试过程中发生异常: {e}")
        is_success = False

    finally:
        # 4. 恢复原始值
        if original_limits is not None:
            print(f"\n[{hand_name}] 恢复原始 Effort Limit...")
            try:
                hand.write_joint_effort_limit(original_limits)
                print(f"  [{hand_name}] 原始值恢复成功")
            except Exception as e:
                print(f"  [{hand_name}] [WARNING] 恢复原始值失败: {e}")

    return is_success


def main(serial_numbers: Optional[list[str]] = None) -> bool:
    """
    主函数

    Args:
        serial_numbers: 灵巧手序列号列表

    Returns:
        是否全部验证通过
    """
    print("=" * 60)
    print("write_joint_effort_limit 功能验证")
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
    all_pass = print_summary(results, "effort_limit 功能验证")
    return all_pass


if __name__ == "__main__":
    parser = create_arg_parser("write_joint_effort_limit 功能验证")
    args = parser.parse_args()

    success = main(serial_numbers=args.serial_numbers)
    sys.exit(0 if success else 1)
