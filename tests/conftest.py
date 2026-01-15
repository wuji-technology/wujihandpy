"""
WujihandPy 测试配置文件

提供:
- 设备连接 fixture 和管理
- 固件版本检测
- 通用错误处理工具
- 测试跳过机制
"""
import pytest
import numpy as np
import wujihandpy as wh


# 存储已检测的设备固件版本信息
_cached_firmware_info = {}


def check_device_connection():
    """
    检查设备是否已连接。

    Returns:
        tuple: (is_connected, hand_instance, error_message)
    """
    try:
        hand = wh.Hand()
        return True, hand, None
    except RuntimeError as e:
        error_msg = str(e)
        if "No such file" in error_msg or "Permission" in error_msg.lower():
            return False, None, "USB 设备未连接或权限不足"
        elif "timeout" in error_msg.lower():
            return False, None, "设备连接超时"
        else:
            return False, None, f"设备连接失败: {e}"
    except Exception as e:
        return False, None, f"未知错误: {e}"


def get_firmware_version(hand):
    """
    获取设备固件版本信息。

    Args:
        hand: Hand 实例

    Returns:
        tuple: (hand_version_str, full_system_version_str 或 None)
    """
    try:
        hand_version = hand.read_firmware_version()
        full_version = None
        try:
            full_version = hand.read_full_system_firmware_version()
        except Exception:
            pass  # 旧固件可能不支持
        return hand_version, full_version
    except Exception as e:
        return None, None


def check_firmware_feature(hand, feature: str) -> tuple[bool, str]:
    """
    检查设备是否支持特定功能。

    Args:
        hand: Hand 实例
        feature: 功能名称 (如 "effort_feedback", "tpdo_proactively_report")

    Returns:
        tuple: (is_supported, reason)
    """
    try:
        hand_version, full_version = get_firmware_version(hand)

        if hand_version is None:
            return False, "无法读取固件版本"

        # 解析版本号
        # hand_version 格式: major.minor.patch[-suffix]
        # full_version 格式: major.minor.patch

        if feature == "effort_feedback":
            # Effort feedback 需要 full_system_version >= 1.2.0
            if full_version is None:
                return False, "固件版本过旧，不支持完整系统版本读取"
            try:
                major, minor = full_version.split('.')[:2]
                if int(major) > 1 or (int(major) == 1 and int(minor) >= 2):
                    return True, None
                else:
                    return False, f"固件版本 {full_version} < 1.2.0，不支持 effort feedback"
            except (ValueError, AttributeError):
                return False, f"无法解析固件版本: {full_version}"

        elif feature == "realtime_controller":
            # 实时控制器需要固件支持
            if full_version is not None:
                return True, None  # 支持完整版本的固件都支持
            # 对于旧固件，尝试创建控制器
            try:
                ctrl = hand.realtime_controller(
                    enable_upstream=True,
                    filter=wh.filter.LowPass(10.0)
                )
                ctrl.close()
                return True, None
            except Exception as e:
                return False, f"实时控制器创建失败: {e}"

        else:
            return False, f"未知功能: {feature}"

    except Exception as e:
        return False, f"功能检测失败: {e}"


@pytest.fixture(scope="session")
def hand():
    """
    创建 Hand 实例的 fixture。
    需要设备已连接且 USB 权限已配置。

    Returns:
        Hand 实例或 None（如果设备未连接）
    """
    is_connected, hand_instance, error_msg = check_device_connection()

    if not is_connected:
        pytest.skip(f"设备未连接: {error_msg}")
        return None

    # 缓存固件信息
    hand_version, full_version = get_firmware_version(hand_instance)
    sn = hand_instance.get_product_sn()
    _cached_firmware_info[sn] = {
        "hand_version": hand_version,
        "full_version": full_version
    }

    yield hand_instance

    # Hand 对象会在测试后自动销毁


@pytest.fixture
def connected_hand(hand):
    """
    确保设备已连接的 fixture。
    如果设备未连接，会跳过测试。
    """
    if hand is None:
        pytest.skip("设备未连接")
    yield hand


@pytest.fixture
def enabled_hand(connected_hand):
    """
    启用所有关节的 fixture。
    测试前启用关节，测试后禁用。
    """
    connected_hand.write_joint_enabled(True)
    yield connected_hand
    connected_hand.write_joint_enabled(False)


@pytest.fixture
def thumb_joint(connected_hand):
    """获取拇指的第一个关节。"""
    return connected_hand.finger(0).joint(0)


@pytest.fixture
def index_finger(connected_hand):
    """获取食指。"""
    return connected_hand.finger(1)


@pytest.fixture
def valid_position_array():
    """生成有效的 (5,4) 位置数组。"""
    return np.random.uniform(-10, 10, (5, 4)).astype(np.float64)


@pytest.fixture
def valid_single_value():
    """生成有效的单值位置。"""
    return 5.0


@pytest.fixture
def invalid_mask():
    """生成无效的掩码数组（形状错误）。"""
    return np.zeros((3, 3), dtype=bool)


@pytest.fixture
def safe_enabled_hand(connected_hand):
    """
    启用关节并设置安全电流限制的 fixture。
    用于需要电流保护的测试场景。
    """
    connected_hand.write_joint_enabled(True)
    connected_hand.write_joint_effort_limit(1.5)  # 安全限流
    yield connected_hand
    connected_hand.write_joint_enabled(False)


@pytest.fixture
def realtime_controller(connected_hand):
    """
    创建实时控制器的 fixture。
    测试后自动关闭控制器。
    """
    ctrl = connected_hand.realtime_controller(
        enable_upstream=True,
        filter=wh.filter.LowPass(10.0)
    )
    yield ctrl
    ctrl.close()


@pytest.fixture
def effort_supported_hand(connected_hand):
    """
    确保设备支持 effort feedback 功能的 fixture。
    如果不支持，跳过测试。
    """
    is_supported, reason = check_firmware_feature(connected_hand, "effort_feedback")
    if not is_supported:
        pytest.skip(f"设备不支持 effort feedback 功能: {reason}")
    yield connected_hand


@pytest.fixture
def safe_enabled_hand(connected_hand):
    """
    启用关节并设置安全电流限制的 fixture。
    用于需要电流保护的测试场景。
    """
    connected_hand.write_joint_enabled(True)
    connected_hand.write_joint_effort_limit(1.5)  # 安全限流
    yield connected_hand
    connected_hand.write_joint_enabled(False)


@pytest.fixture(scope="session")
def device_info(hand):
    """
    获取设备信息的 fixture（会话级别）。
    返回设备固件版本等基本信息。
    """
    if hand is None:
        return None

    hand_version, full_version = get_firmware_version(hand)
    sn = hand.get_product_sn()

    return {
        "serial_number": sn,
        "hand_version": hand_version,
        "full_system_version": full_version,
        "effort_supported": full_version is not None and
            any(v is not None and v != "" for v in (full_version.split('.')[:2]) if v)
    }


def handle_sdk_exception(operation_name: str, exception: Exception) -> str:
    """
    处理 SDK 异常，返回友好的错误信息。

    Args:
        operation_name: 操作名称
        exception: 捕获的异常

    Returns:
        str: 格式化的错误信息
    """
    error_type = type(exception).__name__
    error_msg = str(exception)

    # 针对特定错误类型提供更友好的消息
    if "timeout" in error_msg.lower() or error_type == "TimeoutError":
        return f"{operation_name} 超时: 可能设备响应缓慢或连接不稳定"
    elif "permission" in error_msg.lower() or "access" in error_msg.lower():
        return f"{operation_name} 权限不足: 请检查 USB 设备权限配置"
    elif "no such file" in error_msg.lower() or "not found" in error_msg.lower():
        return f"{operation_name} 设备未找到: 请检查设备是否已连接"
    elif "Effort feedback requires firmware version" in error_msg:
        return f"{operation_name} 固件不支持: {error_msg}"
    elif "closed" in error_msg.lower() or "invalid" in error_msg.lower():
        return f"{operation_name} 资源已关闭或无效: {error_msg}"
    else:
        return f"{operation_name} 失败 ({error_type}): {error_msg}"


@pytest.fixture
def zero_position_array():
    """生成全零的 (5,4) 位置数组。"""
    return np.zeros((5, 4), dtype=np.float64)


@pytest.fixture
def small_random_action():
    """生成小幅度随机动作 (5,4) 数组。"""
    return np.random.uniform(-0.05, 0.05, (5, 4)).astype(np.float64)


def pytest_configure(config):
    """
    pytest 配置钩子。
    """
    config.addinivalue_line(
        "markers", "P0: Must-pass tests for basic functionality"
    )
    config.addinivalue_line(
        "markers", "P1: Recommended tests for complete functionality"
    )
    config.addinivalue_line(
        "markers", "P2: Optional tests for edge cases"
    )


def pytest_collection_modifyitems(config, items):
    """
    修改测试项目，根据优先级排序。
    """
    # 按优先级排序测试用例
    priority_order = {"P0": 0, "P1": 1, "P2": 2}

    def get_priority(item):
        for marker in item.iter_markers():
            if marker.name in priority_order:
                return priority_order[marker.name]
        return 3  # 未标记的优先级最低

    items.sort(key=get_priority)
