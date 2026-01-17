"""
启用灵巧手所有关节

支持单/双灵巧手：
- 不指定序列号时自动连接第一个设备
- 通过 --sn 参数指定一个或两个序列号
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Optional

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

        print("\n正在启用所有关节...")
        enable_all_hands(hands)

        print("\n操作完成")
        return True

    except Exception as e:
        print(f"操作失败: {e}")
        return False


if __name__ == "__main__":
    parser = create_arg_parser("启用灵巧手所有关节")
    args = parser.parse_args()

    success = main(serial_numbers=args.serial_numbers)
    sys.exit(0 if success else 1)
