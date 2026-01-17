"""
禁用灵巧手所有关节

支持单/双灵巧手：
- 未指定序列号时，可通过自动扫描功能扫描并连接所有可用设备
- 可通过 --sn 参数指定一个或多个序列号，仅连接指定设备；并可配合 --auto-scan 参数控制是否扫描所有设备
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Optional

# 添加 verify 目录到路径以导入 _utils
sys.path.insert(0, str(Path(__file__).parent.parent / "verify"))

from _utils import connect_hands, create_arg_parser, disable_all_hands


def main(serial_numbers: Optional[list[str]] = None, auto_scan: bool = False) -> bool:
    """
    主函数

    Args:
        serial_numbers: 灵巧手序列号列表
        auto_scan: 是否自动扫描所有设备

    Returns:
        是否成功
    """
    try:
        print("连接灵巧手...")
        hands = connect_hands(serial_numbers, auto_scan=auto_scan)

        print("\n正在禁用所有关节...")
        disable_all_hands(hands)

        print("\n操作完成")
        return True

    except Exception as e:
        print(f"操作失败: {e}")
        return False


if __name__ == "__main__":
    parser = create_arg_parser("禁用灵巧手所有关节")
    args = parser.parse_args()

    success = main(serial_numbers=args.serial_numbers, auto_scan=args.auto_scan)
    sys.exit(0 if success else 1)
