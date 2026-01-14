"""
WujihandPy 测试配置文件
"""
import pytest
import numpy as np
import wujihandpy as wh


@pytest.fixture(scope="session")
def hand():
    """
    创建 Hand 实例的 fixture。
    需要设备已连接且 USB 权限已配置。
    """
    try:
        device = wh.Hand()
        yield device
    except Exception as e:
        pytest.skip(f"设备未连接或权限不足: {e}")
    finally:
        pass  # Hand 对象会在测试后自动销毁


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
