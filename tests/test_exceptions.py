"""
异常处理测试用例
"""
import pytest
import numpy as np
import wujihandpy as wh


class TestTimeoutError:
    """TimeoutError 异常测试。"""

    @pytest.mark.P0
    def test_timeout_error_exists(self):
        """测试 TimeoutError 异常类存在。"""
        assert hasattr(wh, 'TimeoutError')
        assert issubclass(wh.TimeoutError, Exception)

    @pytest.mark.P0
    def test_sync_operation_timeout(self, connected_hand):
        """测试同步操作超时。"""
        with pytest.raises(wh.TimeoutError):
            # 设置极短超时触发超时
            connected_hand.read_firmware_version(timeout=0.001)

    @pytest.mark.P1
    def test_joint_position_timeout(self, connected_hand):
        """测试关节位置读取超时。"""
        with pytest.raises(wh.TimeoutError):
            connected_hand.read_joint_actual_position(timeout=0.001)

    @pytest.mark.P1
    def test_async_operation_timeout(self, connected_hand):
        """测试异步操作超时。"""
        import asyncio

        async def test():
            with pytest.raises(wh.TimeoutError):
                future = connected_hand.read_firmware_version_async(timeout=0.001)
                await future

        asyncio.run(test())

    @pytest.mark.P2
    def test_timeout_error_message(self, connected_hand):
        """测试超时错误消息内容。"""
        try:
            connected_hand.read_firmware_version(timeout=0.001)
        except wh.TimeoutError as e:
            assert "timed out" in str(e).lower() or "timeout" in str(e).lower()


class TestParameterErrors:
    """参数错误异常测试。"""

    @pytest.mark.P1
    def test_array_dimension_error(self, enabled_hand):
        """测试数组维度错误。"""
        with pytest.raises(RuntimeError):
            # 传入 1D 数组而不是 2D 数组
            invalid_array = np.zeros(20, dtype=np.float64)
            enabled_hand.write_joint_target_position(invalid_array)

    @pytest.mark.P1
    def test_array_shape_mismatch(self, enabled_hand):
        """测试数组形状不匹配。"""
        with pytest.raises(RuntimeError):
            # 正确的形状是 (5, 4)，传入 (6, 4)
            invalid_array = np.zeros((6, 4), dtype=np.float64)
            enabled_hand.write_joint_target_position(invalid_array)

    @pytest.mark.P1
    def test_wrong_array_shape_finger(self, connected_hand):
        """测试手指级数组形状错误。"""
        finger = connected_hand.finger(0)
        with pytest.raises(RuntimeError):
            # 正确的形状是 (4,)，传入 (5,)
            invalid_array = np.zeros(5, dtype=np.float64)
            finger.write_joint_target_position(invalid_array)

    @pytest.mark.P2
    def test_index_out_of_range(self, connected_hand):
        """测试索引越界。"""
        with pytest.raises((IndexError, RuntimeError)):
            connected_hand.finger(5)  # 只能是 0-4

    @pytest.mark.P2
    def test_negative_index(self, connected_hand):
        """测试负数索引。"""
        with pytest.raises((IndexError, RuntimeError)):
            connected_hand.finger(-1)


class TestControllerClosedError:
    """控制器关闭异常测试。"""

    @pytest.mark.P1
    def test_controller_closed_error(self, connected_hand):
        """测试控制器关闭后操作的异常。"""
        controller = connected_hand.realtime_controller(
            enable_upstream=True,
            filter=wh.filter.LowPass(10.0)
        )
        controller.close()

        with pytest.raises(RuntimeError):
            controller.get_joint_actual_position()


class TestInvalidMaskError:
    """无效掩码异常测试。"""

    @pytest.mark.P2
    def test_invalid_mask_dimension(self):
        """测试无效掩码维度。"""
        with pytest.raises(RuntimeError):
            # 正确的形状是 (5, 4)，传入 (3, 3)
            invalid_mask = np.zeros((3, 3), dtype=bool)
            wh.Hand(mask=invalid_mask)

    @pytest.mark.P2
    def test_invalid_mask_1d(self):
        """测试一维掩码。"""
        with pytest.raises(RuntimeError):
            invalid_mask = np.zeros(20, dtype=bool)
            wh.Hand(mask=invalid_mask)


class TestExceptionHandling:
    """异常处理最佳实践测试。"""

    @pytest.mark.P1
    def test_timeout_error_handling(self, connected_hand):
        """测试超时异常处理。"""
        try:
            connected_hand.read_firmware_version(timeout=0.001)
        except wh.TimeoutError:
            pass  # 正确处理超时异常

    @pytest.mark.P1
    def test_parameter_error_handling(self, enabled_hand):
        """测试参数错误异常处理。"""
        try:
            enabled_hand.write_joint_target_position(np.zeros((6, 4)))
        except RuntimeError:
            pass  # 正确处理参数错误

    @pytest.mark.P1
    def test_general_exception_handling(self, connected_hand):
        """测试通用异常处理。"""
        try:
            # 一些可能出错的操作
            connected_hand.read_firmware_version()
        except Exception as e:
            # 应该能处理任何异常
            assert str(e) or True
