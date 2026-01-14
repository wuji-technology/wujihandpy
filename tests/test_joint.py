"""
Joint 类测试用例
"""
import pytest
import numpy as np
import wujihandpy as wh


class TestJointReadProperties:
    """Joint 只读属性测试。"""

    @pytest.mark.P0
    def test_read_joint_actual_position(self, thumb_joint):
        """测试读取关节位置。"""
        position = thumb_joint.read_joint_actual_position()
        assert position is not None
        assert isinstance(position, (float, np.floating))

    @pytest.mark.P0
    def test_read_joint_temperature(self, thumb_joint):
        """测试读取关节温度。"""
        temp = thumb_joint.read_joint_temperature()
        assert temp is not None
        assert isinstance(temp, float)

    @pytest.mark.P1
    def test_read_joint_bus_voltage(self, thumb_joint):
        """测试读取关节电压。"""
        voltage = thumb_joint.read_joint_bus_voltage()
        assert voltage is not None
        assert isinstance(voltage, float)

    @pytest.mark.P1
    def test_read_joint_error_code(self, thumb_joint):
        """测试读取关节错误码。"""
        error_code = thumb_joint.read_joint_error_code()
        assert error_code is not None
        assert isinstance(error_code, (int, np.integer))

    @pytest.mark.P1
    def test_read_joint_effort_limit(self, thumb_joint):
        """测试读取关节力矩限制。"""
        limit = thumb_joint.read_joint_effort_limit()
        assert limit is not None
        assert isinstance(limit, (float, np.floating))
        # 力矩限制应该是正数（安培）
        assert limit >= 0

    @pytest.mark.P1
    def test_get_joint_actual_position(self, thumb_joint):
        """测试缓存获取关节位置。"""
        # 先读取
        position = thumb_joint.read_joint_actual_position()
        # 再从缓存获取
        cached = thumb_joint.get_joint_actual_position()
        assert cached is not None
        assert isinstance(cached, (float, np.floating))

    @pytest.mark.P1
    def test_read_joint_firmware_version(self, thumb_joint):
        """测试读取关节固件版本。"""
        version = thumb_joint.read_joint_firmware_version()
        assert version is not None
        assert isinstance(version, (int, np.integer))

    @pytest.mark.P1
    def test_read_joint_firmware_date(self, thumb_joint):
        """测试读取关节固件日期。"""
        date = thumb_joint.read_joint_firmware_date()
        assert date is not None
        assert isinstance(date, (int, np.integer))


class TestJointWriteProperties:
    """Joint 写入属性测试。"""

    @pytest.fixture
    def enabled_joint(self, thumb_joint):
        """启用关节的 fixture。"""
        thumb_joint.write_joint_enabled(True)
        yield thumb_joint
        thumb_joint.write_joint_enabled(False)

    @pytest.mark.P0
    def test_write_joint_target_position(self, enabled_joint):
        """测试写入目标位置。"""
        enabled_joint.write_joint_target_position(5.0)

    @pytest.mark.P0
    def test_write_joint_enabled_true(self, thumb_joint):
        """测试启用关节。"""
        thumb_joint.write_joint_enabled(True)

    @pytest.mark.P0
    def test_write_joint_enabled_false(self, thumb_joint):
        """测试禁用关节。"""
        # 先启用
        thumb_joint.write_joint_enabled(True)
        # 再禁用
        thumb_joint.write_joint_enabled(False)

    @pytest.mark.P1
    def test_write_joint_effort_limit(self, enabled_joint):
        """测试写入力矩限制。"""
        enabled_joint.write_joint_effort_limit(0.5)  # 0.5A

    @pytest.mark.P1
    def test_write_joint_control_mode(self, enabled_joint):
        """测试写入控制模式。"""
        enabled_joint.write_joint_control_mode(6)

    @pytest.mark.P1
    def test_async_joint_write(self, enabled_joint):
        """测试异步写入。"""
        import asyncio

        async def test():
            future = enabled_joint.write_joint_target_position_async(5.0)
            assert future is not None
            await future

        asyncio.run(test())

    @pytest.mark.P2
    def test_unchecked_joint_write(self, enabled_joint):
        """测试非检查写入。"""
        # 非检查写入应立即返回
        enabled_joint.write_joint_target_position_unchecked(5.0, timeout=0.001)


class TestJointPosition:
    """关节位置相关测试。"""

    @pytest.fixture
    def enabled_joint(self, thumb_joint):
        """启用关节的 fixture。"""
        thumb_joint.write_joint_enabled(True)
        yield thumb_joint
        thumb_joint.write_joint_enabled(False)

    @pytest.mark.P0
    def test_position_read_write(self, enabled_joint):
        """测试位置读写循环。"""
        # 读取当前位置
        initial_pos = enabled_joint.read_joint_actual_position()
        assert initial_pos is not None

        # 写入新位置
        new_pos = 10.0
        enabled_joint.write_joint_target_position(new_pos)

    @pytest.mark.P1
    def test_position_within_limits(self, enabled_joint):
        """测试位置在限制范围内。"""
        # 读取位置限制
        upper = enabled_joint.read_joint_upper_limit()
        lower = enabled_joint.read_joint_lower_limit()

        # 验证限制的合理性
        assert upper is not None
        assert lower is not None
        assert upper > lower

        # 在范围内写入位置
        mid_pos = (upper + lower) / 2
        enabled_joint.write_joint_target_position(mid_pos)


class TestJointAllJoints:
    """测试所有关节。"""

    @pytest.mark.P0
    def test_all_joints_read(self, connected_hand):
        """测试读取所有关节的位置。"""
        for finger_id in range(5):
            finger = connected_hand.finger(finger_id)
            for joint_id in range(4):
                joint = finger.joint(joint_id)
                position = joint.read_joint_actual_position()
                assert position is not None

    @pytest.mark.P1
    def test_all_joints_properties(self, connected_hand):
        """测试所有关节的属性。"""
        for finger_id in range(5):
            finger = connected_hand.finger(finger_id)
            for joint_id in range(4):
                joint = finger.joint(joint_id)

                # 读取各种属性
                assert joint.read_joint_temperature() is not None
                assert joint.read_joint_bus_voltage() is not None
                assert joint.read_joint_error_code() is not None
                assert joint.read_joint_upper_limit() is not None
                assert joint.read_joint_lower_limit() is not None
