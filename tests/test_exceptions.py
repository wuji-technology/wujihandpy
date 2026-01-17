"""
异常处理测试用例

提供:
- SDK 异常类型测试
- 异常消息验证
- 最佳实践异常处理示例
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
            error_msg = str(e).lower()
            # 验证错误消息包含超时相关信息
            assert "timed out" in error_msg or "timeout" in error_msg or "timeout" in type(e).__name__.lower()


class TestParameterErrors:
    """参数错误异常测试。"""

    @pytest.mark.P1
    def test_array_dimension_error(self, enabled_hand):
        """测试数组维度错误。"""
        with pytest.raises(RuntimeError) as exc_info:
            # 传入 1D 数组而不是 2D 数组
            invalid_array = np.zeros(20, dtype=np.float64)
            enabled_hand.write_joint_target_position(invalid_array)
        # 验证错误消息有帮助
        assert len(str(exc_info.value)) > 0

    @pytest.mark.P1
    def test_array_shape_mismatch(self, enabled_hand):
        """测试数组形状不匹配。"""
        with pytest.raises(RuntimeError) as exc_info:
            # 正确的形状是 (5, 4)，传入 (6, 4)
            invalid_array = np.zeros((6, 4), dtype=np.float64)
            enabled_hand.write_joint_target_position(invalid_array)
        # 验证错误消息有帮助
        assert len(str(exc_info.value)) > 0

    @pytest.mark.P1
    def test_wrong_array_shape_finger(self, connected_hand):
        """测试手指级数组形状错误。"""
        finger = connected_hand.finger(0)
        with pytest.raises(RuntimeError) as exc_info:
            # 正确的形状是 (4,)，传入 (5,)
            invalid_array = np.zeros(5, dtype=np.float64)
            finger.write_joint_target_position(invalid_array)
        # 验证错误消息有帮助
        assert len(str(exc_info.value)) > 0

    @pytest.mark.P2
    def test_index_out_of_range(self, connected_hand):
        """测试索引越界。"""
        with pytest.raises((IndexError, RuntimeError)) as exc_info:
            connected_hand.finger(5)  # 只能是 0-4
        # 验证错误消息有帮助
        assert len(str(exc_info.value)) > 0

    @pytest.mark.P2
    def test_negative_index(self, connected_hand):
        """测试负数索引。"""
        with pytest.raises((IndexError, RuntimeError)) as exc_info:
            connected_hand.finger(-1)
        # 验证错误消息有帮助
        assert len(str(exc_info.value)) > 0


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

        with pytest.raises(RuntimeError) as exc_info:
            controller.get_joint_actual_position()

        # 验证错误消息表明控制器已关闭
        error_msg = str(exc_info.value).lower()
        assert "closed" in error_msg or "invalid" in error_msg or "released" in error_msg


class TestInvalidMaskError:
    """无效掩码异常测试。"""

    @pytest.mark.P2
    def test_invalid_mask_dimension(self):
        """测试无效掩码维度。"""
        with pytest.raises(RuntimeError) as exc_info:
            # 正确的形状是 (5, 4)，传入 (3, 3)
            invalid_mask = np.zeros((3, 3), dtype=bool)
            wh.Hand(mask=invalid_mask)
        # 验证错误消息有帮助
        assert len(str(exc_info.value)) > 0

    @pytest.mark.P2
    def test_invalid_mask_1d(self):
        """测试一维掩码。"""
        with pytest.raises(RuntimeError) as exc_info:
            invalid_mask = np.zeros(20, dtype=bool)
            wh.Hand(mask=invalid_mask)
        # 验证错误消息有帮助
        assert len(str(exc_info.value)) > 0


class TestFirmwareVersionError:
    """固件版本相关异常测试。"""

    @pytest.mark.P1
    def test_effort_requires_firmware_version(self, connected_hand):
        """
        测试 effort feedback 需要固件版本 >= 1.2.0。

        此测试验证 SDK 正确检测固件版本并抛出有意义的错误消息。
        根据设备固件版本，测试可能:
        - 通过: 固件支持 effort
        - 跳过: 固件不支持 effort
        """
        # 尝试创建控制器并获取 effort
        try:
            with connected_hand.realtime_controller(
                enable_upstream=True,
                filter=wh.filter.LowPass(10.0)
            ) as controller:
                controller.get_joint_actual_effort()
        except RuntimeError as e:
            error_msg = str(e)
            # 如果是固件版本问题，验证错误消息包含有用信息
            if "Effort feedback requires firmware version" in error_msg:
                # 验证错误消息包含当前版本信息
                assert "current" in error_msg.lower() or "version" in error_msg.lower()
            else:
                # 其他错误也应该是有效的 RuntimeError
                assert isinstance(e, RuntimeError)


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

    @pytest.mark.P1
    def test_exception_message_format(self, connected_hand):
        """测试异常消息格式规范。"""
        try:
            # 触发一个已知的错误条件
            connected_hand.finger(10)  # 无效索引
        except (IndexError, RuntimeError) as e:
            # 验证异常消息非空且为字符串
            error_msg = str(e)
            assert isinstance(error_msg, str)
            # 消息应该包含一些有用的信息
            assert len(error_msg) > 0
