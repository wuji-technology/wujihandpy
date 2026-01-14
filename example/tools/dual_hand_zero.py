"""
灵巧手回归零位

支持单/双灵巧手：
- 不指定序列号时自动连接第一个设备
- 通过 --sn 参数指定一个或两个序列号
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Optional

import numpy as np

# 添加 verify 目录到路径以导入 _utils
sys.path.insert(0, str(Path(__file__).parent.parent / "verify"))

from _utils import connect_hands, create_arg_parser, enable_all_hands


def main(serial_numbers: Optional[list[str]] = None) -> bool:
    """
    主函数

    Args:
        serial_numbers: 灵巧手序列号列表

    Returns:
        是否成功
    """
    try:
        print("连接灵巧手...")
        hands = connect_hands(serial_numbers)

        print("\n启用所有关节...")
        enable_all_hands(hands)

        # 稍作等待以确保使能生效
        time.sleep(0.5)

        print("\n正在回归零位...")
        # 创建 5x4 的全 0 矩阵 (5个手指 x 4个关节)
        zero_positions = np.zeros((5, 4), dtype=np.float64)

        for info in hands:
            info.hand.write_joint_target_position(zero_positions)
            print(f"  {info.name} 零位指令已发送")

        print("\n操作完成")
        return True

    except Exception as e:
        print(f"操作失败: {e}")
        return False


if __name__ == "__main__":
    parser = create_arg_parser("灵巧手回归零位")
    args = parser.parse_args()

    success = main(serial_numbers=args.serial_numbers)
    sys.exit(0 if success else 1)
