"""
Finger 类测试用例

提供:
- Finger 方法测试
- 关节级读写属性测试
- 所有手指测试
- 错误处理测试
"""
import pytest
import numpy as np
import wujihandpy as wh


class TestFingerMethods:
    """Finger 方法测试。"""

    @pytest.mark.P0
    def test_get_joint(self, connected_hand):
        """测试获取指定关节。"""
        finger = connected_hand.finger(0)  # 拇指
        joint = finger.joint(0)
        assert joint is not None

    @pytest.mark.P0
    def test_get_all_joints(self, connected_hand):
        """测试获取所有关节。"""
        finger = connected_hand.finger(1)  # 食指
        for i in range(4):
            joint = finger.joint(i)
            assert joint is not None

    @pytest.mark.P2
    def test_joint_index_out_of_range(self, connected_hand):
        """测试关节索引越界。"""
        finger = connected_hand.finger(0)
        with pytest.raises((IndexError, RuntimeError)):
            finger.joint(4)

    @pytest.mark.P2
    def test_joint_negative_index(self, connected_hand):
        """测试关节负数索引。"""
        finger = connected_hand.finger(0)
        with pytest.raises((IndexError, RuntimeError)):
            finger.joint(-1)


class TestFingerJointReadProperties:
    """Finger 关节级只读属性测试。"""

    @pytest.mark.P0
    def test_read_joint_actual_position(self, connected_hand):
        """测试批量读取单指关节位置。"""
        finger = connected_hand.finger(1)  # 食指
        positions = finger.read_joint_actual_position()
        assert positions is not None
        assert isinstance(positions, np.ndarray)
        assert positions.shape == (4,)

    @pytest.mark.P0
    def test_read_joint_temperature(self, connected_hand):
        """测试批量读取单指关节温度。"""
        finger = connected_hand.finger(1)
        temps = finger.read_joint_temperature()
        assert temps is not None
        assert temps.shape == (4,)

    @pytest.mark.P1
    def test_read_joint_bus_voltage(self, connected_hand):
        """测试批量读取单指关节电压。"""
        finger = connected_hand.finger(1)
        voltages = finger.read_joint_bus_voltage()
        assert voltages is not None
        assert voltages.shape == (4,)

    @pytest.mark.P1
    def test_read_joint_error_code(self, connected_hand):
        """测试批量读取单指关节错误码。"""
        finger = connected_hand.finger(1)
        error_codes = finger.read_joint_error_code()
        assert error_codes is not None
        assert error_codes.shape == (4,)

    @pytest.mark.P1
    def test_read_joint_effort_limit(self, connected_hand):
        """测试批量读取单指关节力矩限制。"""
        finger = connected_hand.finger(1)
        limits = finger.read_joint_effort_limit()
        assert limits is not None
        assert limits.shape == (4,)
        assert np.all(limits >= 0)

    @pytest.mark.P1
    def test_async_joint_position_read(self, connected_hand):
        """测试异步批量读取单指关节位置。"""
        import asyncio

        async def test():
            finger = connected_hand.finger(1)
            future = finger.read_joint_actual_position_async()
            assert future is not None
            result = await future
            assert result is not None
            assert result.shape == (4,)

        asyncio.run(test())

    @pytest.mark.P1
    def test_get_joint_actual_position(self, connected_hand):
        """测试缓存获取单指关节位置。"""
        finger = connected_hand.finger(1)
        # 先读取
        positions = finger.read_joint_actual_position()
        # 再从缓存获取
        cached = finger.get_joint_actual_position()
        assert cached is not None
        assert cached.shape == (4,)


class TestFingerJointWriteProperties:
    """Finger 关节级写入属性测试。"""

    @pytest.fixture
    def enabled_finger(self, connected_hand):
        """启用单指的 fixture。"""
        finger = connected_hand.finger(1)  # 食指
        finger.write_joint_enabled(True)
        yield finger
        finger.write_joint_enabled(False)

    @pytest.mark.P0
    def test_write_joint_target_position_array(self, enabled_finger):
        """测试批量写入单指关节目标位置。"""
        positions = np.array([5.0, 5.0, 5.0, 5.0], dtype=np.float64)
        enabled_finger.write_joint_target_position(positions)

    @pytest.mark.P0
    def test_write_joint_enabled_array(self, connected_hand):
        """测试批量启用单指关节。"""
        finger = connected_hand.finger(1)
        enabled_array = np.array([True, True, True, True], dtype=bool)
        finger.write_joint_enabled(enabled_array)

    @pytest.mark.P1
    def test_write_joint_effort_limit_array(self, enabled_finger):
        """测试批量写入力矩限制。"""
        limits = np.array([0.5, 0.5, 0.5, 0.5], dtype=np.float64)
        enabled_finger.write_joint_effort_limit(limits)

    @pytest.mark.P2
    def test_write_joint_target_position_invalid_shape(self, enabled_finger):
        """测试数组形状错误。"""
        with pytest.raises(RuntimeError):
            invalid_array = np.zeros((5,), dtype=np.float64)  # 应该是 4 个元素
            enabled_finger.write_joint_target_position(invalid_array)

    @pytest.mark.P1
    def test_async_joint_write(self, enabled_finger):
        """测试异步批量写入。"""
        import asyncio

        async def test():
            positions = np.array([5.0, 5.0, 5.0, 5.0], dtype=np.float64)
            future = enabled_finger.write_joint_target_position_async(positions)
            assert future is not None
            await future

        asyncio.run(test())


class TestFingerAllFingers:
    """测试所有手指。"""

    @pytest.mark.P0
    def test_all_fingers_exist(self, connected_hand):
        """测试所有手指都可以访问。"""
        for finger_id in range(5):
            finger = connected_hand.finger(finger_id)
            assert finger is not None

    @pytest.mark.P1
    def test_each_finger_joints(self, connected_hand):
        """测试每个手指的所有关节。"""
        for finger_id in range(5):
            finger = connected_hand.finger(finger_id)
            for joint_id in range(4):
                joint = finger.joint(joint_id)
                assert joint is not None


class TestFingerErrorHandling:
    """手指错误处理测试。"""

    @pytest.mark.P2
    def test_joint_index_out_of_range(self, connected_hand):
        """测试关节索引越界。"""
        finger = connected_hand.finger(0)
        with pytest.raises((IndexError, RuntimeError)) as exc_info:
            finger.joint(4)
        # 验证错误消息有帮助
        assert len(str(exc_info.value)) > 0

    @pytest.mark.P2
    def test_joint_negative_index(self, connected_hand):
        """测试关节负数索引。"""
        finger = connected_hand.finger(0)
        with pytest.raises((IndexError, RuntimeError)) as exc_info:
            finger.joint(-1)
        # 验证错误消息有帮助
        assert len(str(exc_info.value)) > 0

    @pytest.mark.P2
    def test_invalid_position_shape(self, connected_hand):
        """测试无效位置数组形状。"""
        finger = connected_hand.finger(1)  # 食指
        with pytest.raises(RuntimeError) as exc_info:
            # 正确的形状是 (4,)，传入 (5,)
            invalid_array = np.zeros(5, dtype=np.float64)
            finger.write_joint_target_position(invalid_array)
        # 验证错误消息有帮助
        assert len(str(exc_info.value)) > 0

    @pytest.mark.P2
    def test_finger_operation_after_timeout(self, connected_hand):
        """
        测试超时后的手指操作恢复。

        验证一个操作超时不 影响后续操作。
        """
        finger = connected_hand.finger(1)  # 食指

        # 第一次超时操作
        try:
            finger.read_joint_actual_position(timeout=0.001)
        except wh.TimeoutError:
            pass  # 预期超时

        # 第二次正常操作应该成功
        positions = finger.read_joint_actual_position(timeout=5.0)
        assert positions is not None
        assert positions.shape == (4,)
