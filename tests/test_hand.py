"""
Hand 类测试用例
"""
import pytest
import numpy as np
import wujihandpy as wh


class TestHandConstructor:
    """Hand 构造函数测试。"""

    @pytest.mark.P0
    def test_default_constructor(self):
        """测试默认构造函数，连接第一个可用设备。"""
        hand = wh.Hand()
        assert hand is not None

    @pytest.mark.P1
    def test_constructor_with_serial_number(self):
        """测试指定序列号连接。"""
        # 假设有一个特定序列号的设备
        # hand = wh Hand(serial_number="test_sn")
        # assert hand is not None
        pytest.skip("需要已知序列号的设备")

    @pytest.mark.P2
    def test_constructor_with_usb_pid(self):
        """测试指定 USB PID 连接。"""
        hand = wh.Hand(usb_pid=0x7530)
        assert hand is not None

    @pytest.mark.P2
    def test_constructor_with_mask(self):
        """测试指定关节掩码。"""
        mask = np.zeros((5, 4), dtype=bool)
        mask[0, :] = True  # 只启用拇指
        mask[1, :] = True  # 只启用食指
        hand = wh.Hand(mask=mask)
        assert hand is not None

    @pytest.mark.P0
    def test_no_device_connected(self):
        """测试无设备连接时抛出 TimeoutError。"""
        pytest.skip("需要实际断开设备测试")

    @pytest.mark.P1
    def test_insufficient_permissions(self):
        """测试权限不足时的情况。"""
        pytest.skip("需要模拟权限不足场景")

    @pytest.mark.P2
    def test_invalid_mask_shape(self):
        """测试无效掩码形状。"""
        with pytest.raises(RuntimeError):
            invalid_mask = np.zeros((3, 3), dtype=bool)
            wh.Hand(mask=invalid_mask)


class TestHandReadOnlyProperties:
    """Hand 只读属性测试。"""

    @pytest.mark.P0
    def test_read_firmware_version(self, connected_hand):
        """测试读取固件版本。"""
        version = connected_hand.read_firmware_version()
        assert version is not None
        assert isinstance(version, (int, np.integer))

    @pytest.mark.P0
    def test_read_temperature(self, connected_hand):
        """测试读取温度。"""
        temp = connected_hand.read_temperature()
        assert temp is not None
        assert isinstance(temp, float)

    @pytest.mark.P0
    def test_read_input_voltage(self, connected_hand):
        """测试读取输入电压。"""
        voltage = connected_hand.read_input_voltage()
        assert voltage is not None
        assert isinstance(voltage, float)

    @pytest.mark.P0
    def test_read_handedness(self, connected_hand):
        """测试读取手性。"""
        handedness = connected_hand.read_handedness()
        assert handedness is not None
        assert isinstance(handedness, (int, np.integer))
        # 手性应该是 0 (左手) 或 1 (右手)
        assert handedness in [0, 1]

    @pytest.mark.P1
    def test_read_system_time(self, connected_hand):
        """测试读取系统时间。"""
        system_time = connected_hand.read_system_time()
        assert system_time is not None
        assert isinstance(system_time, (int, np.integer))

    @pytest.mark.P1
    def test_async_read_firmware_version(self, connected_hand):
        """测试异步读取固件版本。"""
        import asyncio

        async def test():
            future = connected_hand.read_firmware_version_async()
            assert future is not None
            result = await future
            assert result is not None

        asyncio.run(test())

    @pytest.mark.P1
    def test_unchecked_read(self, connected_hand):
        """测试非检查读取。"""
        # 非检查读取应立即返回，不阻塞
        connected_hand.read_firmware_version_unchecked(timeout=0.5)
        # 不抛出异常即通过

    @pytest.mark.P1
    def test_get_firmware_version(self, connected_hand):
        """测试缓存获取固件版本。"""
        # 先读取
        version = connected_hand.read_firmware_version()
        # 再从缓存获取
        cached_version = connected_hand.get_firmware_version()
        assert cached_version == version


class TestHandJointReadProperties:
    """Hand 关节级只读属性测试。"""

    @pytest.mark.P0
    def test_read_joint_actual_position(self, connected_hand):
        """测试批量读取关节位置。"""
        positions = connected_hand.read_joint_actual_position()
        assert positions is not None
        assert isinstance(positions, np.ndarray)
        assert positions.shape == (5, 4)

    @pytest.mark.P0
    def test_read_joint_temperature(self, connected_hand):
        """测试批量读取关节温度。"""
        temps = connected_hand.read_joint_temperature()
        assert temps is not None
        assert isinstance(temps, np.ndarray)
        assert temps.shape == (5, 4)

    @pytest.mark.P1
    def test_read_joint_bus_voltage(self, connected_hand):
        """测试批量读取关节电压。"""
        voltages = connected_hand.read_joint_bus_voltage()
        assert voltages is not None
        assert voltages.shape == (5, 4)

    @pytest.mark.P1
    def test_read_joint_error_code(self, connected_hand):
        """测试批量读取错误码。"""
        error_codes = connected_hand.read_joint_error_code()
        assert error_codes is not None
        assert error_codes.shape == (5, 4)

    @pytest.mark.P1
    def test_read_joint_effort_limit(self, connected_hand):
        """测试批量读取力矩限制。"""
        limits = connected_hand.read_joint_effort_limit()
        assert limits is not None
        assert limits.shape == (5, 4)
        # 力矩限制应该是正数（安培）
        assert np.all(limits >= 0)

    @pytest.mark.P1
    def test_async_joint_position_read(self, connected_hand):
        """测试异步批量读取关节位置。"""
        import asyncio

        async def test():
            future = connected_hand.read_joint_actual_position_async()
            assert future is not None
            result = await future
            assert result is not None
            assert result.shape == (5, 4)

        asyncio.run(test())

    @pytest.mark.P1
    def test_get_joint_actual_position(self, connected_hand):
        """测试缓存获取批量关节位置。"""
        # 先读取
        positions = connected_hand.read_joint_actual_position()
        # 再从缓存获取
        cached = connected_hand.get_joint_actual_position()
        assert cached is not None
        assert cached.shape == (5, 4)


class TestHandJointWriteProperties:
    """Hand 关节级写入属性测试。"""

    @pytest.mark.P0
    def test_write_joint_target_position_single_value(self, enabled_hand, valid_single_value):
        """测试单值写入目标位置。"""
        enabled_hand.write_joint_target_position(valid_single_value)

    @pytest.mark.P0
    def test_write_joint_target_position_array(self, enabled_hand, valid_position_array):
        """测试数组写入目标位置。"""
        enabled_hand.write_joint_target_position(valid_position_array)

    @pytest.mark.P0
    def test_write_joint_enabled_true(self, connected_hand):
        """测试启用关节。"""
        connected_hand.write_joint_enabled(True)

    @pytest.mark.P0
    def test_write_joint_enabled_false(self, connected_hand):
        """测试禁用关节。"""
        # 先启用
        connected_hand.write_joint_enabled(True)
        # 再禁用
        connected_hand.write_joint_enabled(False)

    @pytest.mark.P1
    def test_write_joint_effort_limit(self, enabled_hand):
        """测试写入力矩限制。"""
        enabled_hand.write_joint_effort_limit(0.5)  # 0.5A

    @pytest.mark.P1
    def test_write_joint_control_mode(self, enabled_hand):
        """测试写入控制模式。"""
        enabled_hand.write_joint_control_mode(6)

    @pytest.mark.P2
    def test_write_joint_target_position_invalid_shape(self, enabled_hand):
        """测试数组形状错误。"""
        with pytest.raises(RuntimeError):
            invalid_array = np.zeros((6, 4), dtype=np.float64)
            enabled_hand.write_joint_target_position(invalid_array)

    @pytest.mark.P1
    def test_async_joint_write(self, enabled_hand, valid_position_array):
        """测试异步写入。"""
        import asyncio

        async def test():
            future = enabled_hand.write_joint_target_position_async(valid_position_array)
            assert future is not None
            await future

        asyncio.run(test())

    @pytest.mark.P2
    def test_unchecked_joint_write(self, enabled_hand):
        """测试非检查写入。"""
        # 非检查写入应立即返回
        enabled_hand.write_joint_target_position_unchecked(5.0, timeout=0.001)

    @pytest.mark.P1
    def test_position_clamping(self, enabled_hand):
        """测试超出边界位置自动 clamp。"""
        # 写入一个极大的值
        enabled_hand.write_joint_target_position(1000.0)
        # 应该不会抛出异常，而是被 clamp 到上限


class TestHandSpecialMethods:
    """Hand 专用方法测试。"""

    @pytest.mark.P0
    def test_finger(self, connected_hand):
        """测试获取指定手指。"""
        thumb = connected_hand.finger(0)
        assert thumb is not None

    @pytest.mark.P2
    def test_finger_index_out_of_range(self, connected_hand):
        """测试手指索引越界。"""
        with pytest.raises((IndexError, RuntimeError)):
            connected_hand.finger(5)

    @pytest.mark.P0
    def test_realtime_controller(self, connected_hand):
        """测试创建实时控制器。"""
        controller = connected_hand.realtime_controller(
            enable_upstream=True,
            filter=wh.filter.LowPass(10.0)
        )
        assert controller is not None
        controller.close()

    @pytest.mark.P1
    def test_realtime_controller_no_upstream(self, connected_hand):
        """测试创建无上游实时控制器。"""
        controller = connected_hand.realtime_controller(
            enable_upstream=False,
            filter=wh.filter.LowPass(10.0)
        )
        assert controller is not None
        controller.close()

    @pytest.mark.P1
    def test_latency_test(self, connected_hand):
        """测试延迟测试启动和停止。"""
        connected_hand.start_latency_test()
        # 短暂等待
        import time
        time.sleep(0.1)
        connected_hand.stop_latency_test()

    @pytest.mark.P1
    def test_get_product_sn(self, connected_hand):
        """测试获取产品序列号。"""
        sn = connected_hand.get_product_sn()
        assert sn is not None
        assert isinstance(sn, str)
        # SN 应该是非空字符串
        assert len(sn) > 0

    @pytest.mark.P2
    def test_raw_sdo_read(self, connected_hand):
        """测试原始 SDO 读。"""
        # 读取固件版本
        result = connected_hand.raw_sdo_read(
            finger_id=0,
            joint_id=0,
            index=0x01,
            sub_index=1
        )
        assert result is not None
        assert isinstance(result, bytes)

    @pytest.mark.P2
    def test_raw_sdo_write(self, connected_hand):
        """测试原始 SDO 写。"""
        # 写入控制模式
        connected_hand.raw_sdo_write(
            finger_id=0,
            joint_id=0,
            index=0x02,
            sub_index=1,
            data=b'\x00\x06'  # 控制模式 6
        )

    @pytest.mark.P2
    def test_raw_sdo_invalid_index(self, connected_hand):
        """测试原始 SDO 无效索引。"""
        with pytest.raises(Exception):  # 可能 TimeoutError 或其他
            connected_hand.raw_sdo_read(
                finger_id=0,
                joint_id=0,
                index=0xFFFF,
                sub_index=1,
                timeout=0.01  # 短超时
            )
