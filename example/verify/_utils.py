"""
验证脚本公共工具模块

提供灵巧手设备连接和多设备管理的通用功能
"""

from __future__ import annotations

import argparse
import ctypes
from dataclasses import dataclass
from typing import Callable, Optional

import wujihandpy

# USB 设备 VID/PID
WUJI_VID = 0x0483
WUJI_PID = 0x7530


def get_hand_name_by_sn(sn: str) -> str:
    """
    根据序列号第6位判断左右手

    规则:
        - L: 左手 (Left)
        - R: 右手 (Right)
        - 其他或无 SN: 未知

    Args:
        sn: 序列号，示例: LQSQJL.260112.004 (第6位是 'L')

    Returns:
        "左手"、"右手" 或 "未知"
    """
    if not sn or len(sn) < 6:
        return "未知"

    sixth_char = sn[5].upper()
    if sixth_char == "L":
        return "左手"
    elif sixth_char == "R":
        return "右手"
    else:
        return "未知"


def scan_hands() -> list[str]:
    """
    扫描所有连接的灵巧手设备并返回序列号列表

    使用 libusb 扫描 USB 设备，查找所有匹配 VID/PID 的设备并读取其序列号

    Returns:
        扫描到的设备序列号列表

    Raises:
        RuntimeError: 扫描失败或 libusb 不可用
    """
    try:
        libusb = ctypes.CDLL("libusb-1.0.so.0")
    except OSError:
        raise RuntimeError("libusb-1.0 不可用，请确保已安装 libusb 开发库")

    # 定义 libusb 设备描述符结构
    class DeviceDescriptor(ctypes.Structure):
        _fields_ = [
            ("bLength", ctypes.c_uint8),
            ("bDescriptorType", ctypes.c_uint8),
            ("bcdUSB", ctypes.c_uint16),
            ("bDeviceClass", ctypes.c_uint8),
            ("bDeviceSubClass", ctypes.c_uint8),
            ("bDeviceProtocol", ctypes.c_uint8),
            ("bMaxPacketSize0", ctypes.c_uint8),
            ("idVendor", ctypes.c_uint16),
            ("idProduct", ctypes.c_uint16),
            ("bcdDevice", ctypes.c_uint16),
            ("iManufacturer", ctypes.c_uint8),
            ("iProduct", ctypes.c_uint8),
            ("iSerialNumber", ctypes.c_uint8),
            ("bNumConfigurations", ctypes.c_uint8),
        ]

    # 初始化 libusb
    libusb.libusb_init(None)

    # 获取设备列表
    device_list = ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))()
    device_count = libusb.libusb_get_device_list(None, ctypes.byref(device_list))
    if device_count < 0:
        raise RuntimeError(f"获取设备列表失败: {device_count}")

    serial_numbers: list[str] = []

    try:
        for i in range(device_count):
            device = device_list[i]

            # 获取设备描述符
            descriptor = DeviceDescriptor()
            ret = libusb.libusb_get_device_descriptor(
                device, ctypes.byref(descriptor)
            )
            if ret != 0:
                continue

            # 检查 VID/PID 是否匹配
            if descriptor.idVendor != WUJI_VID:
                continue
            if WUJI_PID >= 0 and descriptor.idProduct != WUJI_PID:
                continue

            # 检查是否有序列号
            if descriptor.iSerialNumber == 0:
                continue

            # 打开设备以读取序列号
            handle = ctypes.c_void_p()
            ret = libusb.libusb_open(device, ctypes.byref(handle))
            if ret != 0:
                continue

            # 读取序列号
            serial_buf = ctypes.create_string_buffer(256)
            n = libusb.libusb_get_string_descriptor_ascii(
                handle, descriptor.iSerialNumber, serial_buf, 255
            )
            libusb.libusb_close(handle)

            if n > 0:
                sn = serial_buf.value.decode("utf-8", errors="ignore")
                if sn and sn not in serial_numbers:
                    serial_numbers.append(sn)

    finally:
        libusb.libusb_free_device_list(device_list, 1)
        libusb.libusb_exit(None)

    return serial_numbers


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
        help="灵巧手序列号，可指定多个。不指定时自动扫描所有设备",
    )
    parser.add_argument(
        "--scan",
        "--auto",
        dest="auto_scan",
        action="store_true",
        help="自动扫描并连接所有检测到的灵巧手设备",
    )
    return parser


def connect_hands(
    serial_numbers: Optional[list[str]] = None,
    auto_scan: bool = False,
) -> list[HandInfo]:
    """
    连接灵巧手设备

    Args:
        serial_numbers: 序列号列表，None 时使用自动扫描
        auto_scan: 是否自动扫描所有设备

    Returns:
        连接成功的设备信息列表

    Raises:
        RuntimeError: 无法连接任何设备
    """
    hands: list[HandInfo] = []

    # 如果 auto_scan 为 True 或未指定序列号，则扫描设备
    if auto_scan or not serial_numbers:
        print("扫描灵巧手设备...")
        try:
            scanned_sns = scan_hands()
        except RuntimeError as e:
            # 扫描失败，回退到单设备连接
            print(f"  [WARNING] 自动扫描失败: {e}")
            print("  回退到单设备连接...")
            scanned_sns = []

        if scanned_sns:
            serial_numbers = scanned_sns
            print(f"  扫描到 {len(scanned_sns)} 个设备: {scanned_sns}")
        elif serial_numbers:
            # 已指定序列号，使用指定序列号
            pass
        else:
            # 未指定序列号且扫描失败，尝试连接默认设备
            try:
                hand = wujihandpy.Hand()
                sn = hand.get_product_sn()
                name = get_hand_name_by_sn(sn)
                hands.append(HandInfo(name=name, hand=hand, serial_number=sn))
                print(f"  已连接: {name} (SN: {sn})")
            except Exception as e:
                raise RuntimeError(f"无法连接灵巧手设备: {e}") from e
            return hands

    # 按序列号连接
    if serial_numbers:
        for sn in serial_numbers:
            try:
                hand = wujihandpy.Hand(serial_number=sn)
                actual_sn = hand.get_product_sn()
                name = get_hand_name_by_sn(actual_sn)
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
