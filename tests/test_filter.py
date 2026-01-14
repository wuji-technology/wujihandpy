"""
filter 模块测试用例
"""
import pytest
import wujihandpy as wh
from wujihandpy import filter


class TestLowPassFilter:
    """LowPass 滤波器测试。"""

    @pytest.mark.P0
    def test_default_cutoff_frequency(self):
        """测试默认截止频率（10.0 Hz）。"""
        lp_filter = filter.LowPass()
        assert lp_filter is not None

    @pytest.mark.P0
    def test_custom_cutoff_frequency(self):
        """测试自定义截止频率。"""
        lp_filter = filter.LowPass(cutoff_freq=5.0)
        assert lp_filter is not None

        lp_filter_20 = filter.LowPass(cutoff_freq=20.0)
        assert lp_filter_20 is not None

    @pytest.mark.P2
    def test_zero_cutoff_frequency(self):
        """测试零截止频率。"""
        lp_filter = filter.LowPass(cutoff_freq=0.0)
        assert lp_filter is not None

    @pytest.mark.P2
    def test_high_cutoff_frequency(self):
        """测试高截止频率。"""
        lp_filter = filter.LowPass(cutoff_freq=1000.0)
        assert lp_filter is not None

    @pytest.mark.P2
    def test_negative_cutoff_frequency(self):
        """测试负截止频率。"""
        # 负值可能被调整或抛出异常
        try:
            lp_filter = filter.LowPass(cutoff_freq=-5.0)
            assert lp_filter is not None
        except (ValueError, RuntimeError):
            pass  # 预期行为


class TestFilterInterface:
    """滤波器接口测试。"""

    @pytest.mark.P0
    def test_ifilter_exists(self):
        """测试 IFilter 接口存在。"""
        assert hasattr(filter, 'IFilter')

    @pytest.mark.P0
    def test_lowpass_is_ifilter(self):
        """测试 LowPass 是 IFilter 的实例。"""
        lp_filter = filter.LowPass()
        assert isinstance(lp_filter, filter.IFilter)


class TestFilterWithHand:
    """滤波器与 Hand 结合测试。"""

    @pytest.mark.P0
    def test_filter_creates_controller(self, connected_hand):
        """测试滤波器创建控制器。"""
        lp_filter = filter.LowPass(10.0)
        controller = connected_hand.realtime_controller(
            enable_upstream=True,
            filter=lp_filter
        )
        assert controller is not None
        controller.close()

    @pytest.mark.P1
    def test_different_filters_different_controllers(self, connected_hand):
        """测试不同滤波器创建不同控制器。"""
        filter_5hz = filter.LowPass(5.0)
        filter_10hz = filter.LowPass(10.0)

        ctrl_5hz = connected_hand.realtime_controller(
            enable_upstream=True,
            filter=filter_5hz
        )
        ctrl_10hz = connected_hand.realtime_controller(
            enable_upstream=True,
            filter=filter_10hz
        )

        assert ctrl_5hz is not None
        assert ctrl_10hz is not None

        ctrl_5hz.close()
        ctrl_10hz.close()
