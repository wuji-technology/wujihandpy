"""
验证脚本公共工具模块

提供灵巧手设备连接和多设备管理的通用功能
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from typing import Callable, Optional

import wujihandpy


@dataclass
class HandInfo:
    """灵巧手设备信息"""

    name: str  # 设备名称 (如 "左手", "右手", "手1")
    hand: wujihandpy.Hand  # 设备实例
    serial_number: str  # 序列号


def create_arg_parser(description: str) -> argparse.ArgumentParser:
    """
    创建支持单/双手配置的命令行参数解析器

    Args:
        description: 脚本描述

    Returns:
        配置好的 ArgumentParser
    """
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument(
        "--sn",
        "--serial",
        dest="serial_numbers",
        nargs="+",
        metavar="SN",
        help="灵巧手序列号，可指定 1-2 个。不指定时自动连接第一个检测到的设备",
    )
    return parser


def connect_hands(
    serial_numbers: Optional[list[str]] = None,
) -> list[HandInfo]:
    """
    连接灵巧手设备

    Args:
        serial_numbers: 序列号列表，None 或空列表时自动连接第一个设备

    Returns:
        连接成功的设备信息列表

    Raises:
        RuntimeError: 无法连接任何设备
    """
    hands: list[HandInfo] = []

    if not serial_numbers:
        # 未指定序列号，尝试连接默认设备
        try:
            hand = wujihandpy.Hand()
            sn = hand.get_product_sn()
            hands.append(HandInfo(name="灵巧手", hand=hand, serial_number=sn))
            print(f"  已连接: 灵巧手 (SN: {sn})")
        except Exception as e:
            raise RuntimeError(f"无法连接灵巧手设备: {e}") from e
    else:
        # 按指定序列号连接
        names = ["左手", "右手"] if len(serial_numbers) == 2 else [f"手{i+1}" for i in range(len(serial_numbers))]
        for i, sn in enumerate(serial_numbers):
            try:
                hand = wujihandpy.Hand(serial_number=sn)
                actual_sn = hand.get_product_sn()
                name = names[i] if i < len(names) else f"手{i+1}"
                hands.append(HandInfo(name=name, hand=hand, serial_number=actual_sn))
                print(f"  已连接: {name} (SN: {actual_sn})")
            except Exception as e:
                print(f"  [WARNING] 无法连接序列号 {sn} 的设备: {e}")

    if not hands:
        raise RuntimeError("未能连接任何灵巧手设备")

    return hands


def for_each_hand(
    hands: list[HandInfo],
    operation: Callable[[HandInfo], bool],
    operation_name: str = "操作",
) -> dict[str, bool]:
    """
    对每只手执行操作

    Args:
        hands: 设备列表
        operation: 操作函数，接受 HandInfo，返回是否成功
        operation_name: 操作名称（用于打印）

    Returns:
        每只手的操作结果 {name: success}
    """
    results: dict[str, bool] = {}

    for info in hands:
        if len(hands) > 1:
            print(f"\n{'='*40}")
            print(f"{operation_name}: {info.name} (SN: {info.serial_number})")
            print("=" * 40)

        try:
            success = operation(info)
            results[info.name] = success
        except Exception as e:
            print(f"  [ERROR] {info.name} {operation_name}失败: {e}")
            results[info.name] = False

    return results


def enable_all_hands(hands: list[HandInfo]) -> None:
    """启用所有灵巧手的关节"""
    for info in hands:
        info.hand.write_joint_enabled(True)
        print(f"  {info.name} 关节已启用")


def disable_all_hands(hands: list[HandInfo]) -> None:
    """禁用所有灵巧手的关节"""
    for info in hands:
        try:
            info.hand.write_joint_enabled(False)
            print(f"  {info.name} 关节已禁用")
        except Exception:
            pass


def print_summary(results: dict[str, bool], test_name: str = "验证") -> bool:
    """
    打印测试结果摘要

    Args:
        results: 每只手的测试结果
        test_name: 测试名称

    Returns:
        是否全部通过
    """
    print("\n" + "=" * 60)
    print(f"{test_name}结果摘要")
    print("=" * 60)

    all_pass = True
    for name, success in results.items():
        status = "PASS" if success else "FAIL"
        print(f"  {name}: {status}")
        if not success:
            all_pass = False

    print("-" * 60)
    overall = "PASS" if all_pass else "FAIL"
    print(f"  总体结果: {overall}")
    print("=" * 60)

    return all_pass


def check_effort_support(hand: wujihandpy.Hand, hand_name: str = "设备") -> bool:
    """
    检测设备是否支持 effort feedback 功能

    Effort 功能需要固件版本 >= 1.2.0

    Args:
        hand: 灵巧手设备实例
        hand_name: 设备名称（用于打印）

    Returns:
        是否支持 effort 功能
    """
    try:
        # 尝试创建一个实时控制器来检测
        # 如果不支持会抛出异常
        with hand.realtime_controller(
            enable_upstream=True, filter=wujihandpy.filter.LowPass(cutoff_freq=100.0)
        ) as controller:
            # 尝试调用 get_joint_actual_effort()
            _ = controller.get_joint_actual_effort()
            return True
    except RuntimeError as e:
        if "Effort feedback requires firmware version" in str(e):
            print(f"  [WARNING] {hand_name} 不支持 effort 功能: {e}")
            return False
        # 其他异常，重新抛出
        raise
