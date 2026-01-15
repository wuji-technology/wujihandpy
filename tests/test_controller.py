"""
IController 实时控制器测试用例

提供:
- 基础控制器功能测试
- 固件版本检测机制
- 优雅的错误处理
"""
import pytest
import numpy as np
import time
import asyncio
import wujihandpy as wh


class TestIControllerBasic:
    """IController 基础功能测试。"""

    @pytest.fixture
    def controller(self, connected_hand):
        """创建实时控制器的 fixture。"""
        ctrl = connected_hand.realtime_controller(
            enable_upstream=True,
            filter=wh.filter.LowPass(10.0)
        )
        yield ctrl
        ctrl.close()

    @pytest.mark.P0
    def test_get_joint_actual_position(self, controller):
        """测试获取所有关节实际位置。"""
        positions = controller.get_joint_actual_position()
        assert positions is not None
        assert isinstance(positions, np.ndarray)
        assert positions.shape == (5, 4)

    @pytest.mark.P0
    def test_get_joint_actual_effort(self, effort_supported_hand):
        """
        测试获取所有关节实际力矩。

        注意: 此测试需要固件版本 >= 1.2.0
        如果固件不支持，测试会被跳过。
        """
        # 使用专门的 fixture 确保固件支持
        with effort_supported_hand.realtime_controller(
            enable_upstream=True,
            filter=wh.filter.LowPass(10.0)
        ) as controller:
            efforts = controller.get_joint_actual_effort()
            assert efforts is not None
            assert isinstance(efforts, np.ndarray)
            assert efforts.shape == (5, 4)

    @pytest.mark.P0
    def test_set_joint_target_position(self, controller):
        """测试设置所有关节目标位置。"""
        target = np.zeros((5, 4), dtype=np.float64)
        controller.set_joint_target_position(target)

    @pytest.mark.P2
    def test_set_joint_target_position_invalid_shape(self, controller):
        """测试设置目标位置时数组形状错误。"""
        with pytest.raises(RuntimeError):
            invalid_target = np.zeros((4, 4), dtype=np.float64)
            controller.set_joint_target_position(invalid_target)

    @pytest.mark.P0
    def test_close_controller(self, connected_hand):
        """测试关闭控制器。"""
        controller = connected_hand.realtime_controller(
            enable_upstream=True,
            filter=wh.filter.LowPass(10.0)
        )
        controller.close()
        # 关闭后不应抛出异常

    @pytest.mark.P1
    def test_close_then_operation(self, connected_hand):
        """测试关闭后操作抛出异常。"""
        controller = connected_hand.realtime_controller(
            enable_upstream=True,
            filter=wh.filter.LowPass(10.0)
        )
        controller.close()
        with pytest.raises(RuntimeError):
            controller.get_joint_actual_position()

    @pytest.mark.P0
    def test_context_manager(self, connected_hand):
        """测试上下文管理器。"""
        with connected_hand.realtime_controller(
            enable_upstream=True,
            filter=wh.filter.LowPass(10.0)
        ) as controller:
            assert controller is not None
            positions = controller.get_joint_actual_position()
            assert positions.shape == (5, 4)

    @pytest.mark.P2
    def test_double_close(self, connected_hand):
        """测试重复关闭。"""
        controller = connected_hand.realtime_controller(
            enable_upstream=True,
            filter=wh.filter.LowPass(10.0)
        )
        controller.close()
        # 第二次关闭不应抛出异常
        controller.close()


class TestIControllerNoUpstream:
    """无上游数据传输的控制器测试。"""

    @pytest.fixture
    def controller_no_upstream(self, connected_hand):
        """创建无上游控制器的 fixture。"""
        ctrl = connected_hand.realtime_controller(
            enable_upstream=False,
            filter=wh.filter.LowPass(10.0)
        )
        yield ctrl
        ctrl.close()

    @pytest.mark.P1
    def test_set_position_no_upstream(self, controller_no_upstream):
        """测试无上游控制器设置位置。"""
        target = np.zeros((5, 4), dtype=np.float64)
        controller_no_upstream.set_joint_target_position(target)

    @pytest.mark.P1
    def test_get_effort_requires_upstream(self, controller_no_upstream):
        """
        测试无上游时获取力矩会抛出异常。

        注意: 即使固件支持 effort feedback，无上游也无法获取。
        此测试在两种情况下都会通过:
        1. 固件不支持 effort → SDK 抛出固件版本异常
        2. 固件支持但无上游 → 抛出运行时异常
        """
        with pytest.raises(Exception) as exc_info:
            controller_no_upstream.get_joint_actual_effort()

        # 验证异常消息包含有用的信息
        error_msg = str(exc_info.value)
        # 可能是 "Effort feedback requires firmware version" 或 "upstream" 相关错误
        assert "Effort feedback requires firmware version" in error_msg or \
               "upstream" in error_msg.lower() or \
               "requires" in error_msg.lower()


class TestIControllerRealTimeControl:
    """实时控制器性能测试。"""

    @pytest.fixture
    def controller(self, connected_hand):
        """创建实时控制器并启用关节。"""
        # 先启用关节
        connected_hand.write_joint_enabled(True)
        ctrl = connected_hand.realtime_controller(
            enable_upstream=True,
            filter=wh.filter.LowPass(10.0)
        )
        yield ctrl
        ctrl.close()
        connected_hand.write_joint_enabled(False)

    @pytest.mark.P1
    def test_khz_control_loop(self, controller):
        """测试 1kHz 控制循环。"""
        target = np.zeros((5, 4), dtype=np.float64)

        # 测量 1000 次循环的时间
        iterations = 1000
        start_time = time.perf_counter()

        for _ in range(iterations):
            controller.set_joint_target_position(target)

        elapsed = time.perf_counter() - start_time
        actual_freq = iterations / elapsed

        # 期望频率 >= 800Hz（允许一定误差）
        assert actual_freq >= 800, f"实际控制频率 {actual_freq:.0f}Hz 低于 800Hz"

    @pytest.mark.P1
    def test_position_update_rate(self, controller):
        """测试位置更新速率。"""
        # 发送一系列不同的位置
        for i in range(100):
            target = np.full((5, 4), float(i), dtype=np.float64)
            controller.set_joint_target_position(target)

    @pytest.mark.P1
    def test_continuous_control(self, controller):
        """测试持续控制。"""
        # 控制 1 秒钟
        target = np.zeros((5, 4), dtype=np.float64)

        start = time.perf_counter()
        while time.perf_counter() - start < 1.0:
            controller.set_joint_target_position(target)


class TestIControllerWithFilter:
    """滤波器效果测试。"""

    @pytest.fixture
    def controller_with_filter(self, connected_hand):
        """创建带滤波器的控制器。"""
        connected_hand.write_joint_enabled(True)
        ctrl = connected_hand.realtime_controller(
            enable_upstream=True,
            filter=wh.filter.LowPass(5.0)  # 5Hz 截止频率
        )
        yield ctrl
        ctrl.close()
        connected_hand.write_joint_enabled(False)

    @pytest.fixture
    def controller_without_filter(self, connected_hand):
        """创建不带滤波器的控制器。"""
        connected_hand.write_joint_enabled(True)
        # 使用极高截止频率模拟无滤波效果
        ctrl = connected_hand.realtime_controller(
            enable_upstream=True,
            filter=wh.filter.LowPass(1000.0)  # 1kHz 截止频率
        )
        yield ctrl
        ctrl.close()
        connected_hand.write_joint_enabled(False)

    @pytest.mark.P1
    def test_controller_with_different_cutoff(self):
        """测试不同截止频率的控制器。"""
        # 5Hz 截止频率
        ctrl_5hz = wh.filter.LowPass(5.0)
        assert ctrl_5hz is not None

        # 10Hz 截止频率
        ctrl_10hz = wh.filter.LowPass(10.0)
        assert ctrl_10hz is not None

        # 20Hz 截止频率
        ctrl_20hz = wh.filter.LowPass(20.0)
        assert ctrl_20hz is not None


class TestIControllerPositionRange:
    """控制器位置范围测试。"""

    @pytest.fixture
    def controller(self, connected_hand):
        """创建实时控制器。"""
        connected_hand.write_joint_enabled(True)
        ctrl = connected_hand.realtime_controller(
            enable_upstream=True,
            filter=wh.filter.LowPass(10.0)
        )
        yield ctrl
        ctrl.close()
        connected_hand.write_joint_enabled(False)

    @pytest.mark.P1
    def test_zero_position(self, controller):
        """测试设置零位置。"""
        target = np.zeros((5, 4), dtype=np.float64)
        controller.set_joint_target_position(target)

    @pytest.mark.P1
    def test_random_positions(self, controller):
        """测试设置随机位置。"""
        for _ in range(10):
            target = np.random.uniform(-20, 20, (5, 4)).astype(np.float64)
            controller.set_joint_target_position(target)

    @pytest.mark.P1
    def test_sequential_positions(self, controller):
        """测试连续位置变化。"""
        # 从 -10 到 10 渐变
        for i in range(21):
            target = np.full((5, 4), float(i - 10), dtype=np.float64)
            controller.set_joint_target_position(target)
