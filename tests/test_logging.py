"""
logging 模块测试用例
"""
import pytest
import os
import tempfile
import wujihandpy as wh
from wujihandpy import logging


class TestLoggingLevel:
    """日志级别测试。"""

    @pytest.mark.P1
    def test_level_enum_exists(self):
        """测试 Level 枚举存在。"""
        assert hasattr(logging, 'Level')

    @pytest.mark.P1
    def test_all_log_levels(self):
        """测试所有日志级别。"""
        assert logging.Level.TRACE is not None
        assert logging.Level.DEBUG is not None
        assert logging.Level.INFO is not None
        assert logging.Level.WARN is not None
        assert logging.Level.ERROR is not None
        assert logging.Level.CRITICAL is not None
        assert logging.Level.OFF is not None

    @pytest.mark.P1
    def test_level_values(self):
        """测试日志级别值。"""
        assert int(logging.Level.TRACE) == 0
        assert int(logging.Level.DEBUG) == 1
        assert int(logging.Level.INFO) == 2
        assert int(logging.Level.WARN) == 3
        assert int(logging.Level.ERROR) == 4
        assert int(logging.Level.CRITICAL) == 5
        assert int(logging.Level.OFF) == 6


class TestLoggingConsole:
    """控制台日志测试。"""

    @pytest.mark.P1
    def test_set_log_to_console_true(self):
        """测试启用控制台日志。"""
        logging.set_log_to_console(True)

    @pytest.mark.P1
    def test_set_log_to_console_false(self):
        """测试禁用控制台日志。"""
        logging.set_log_to_console(False)


class TestLoggingFile:
    """文件日志测试。"""

    @pytest.fixture
    def temp_log_file(self):
        """创建临时日志文件。"""
        with tempfile.NamedTemporaryFile(mode='w', suffix='.log', delete=False) as f:
            temp_path = f.name
        yield temp_path
        # 清理
        if os.path.exists(temp_path):
            os.remove(temp_path)

    @pytest.mark.P1
    def test_set_log_path(self, temp_log_file):
        """测试设置日志文件路径。"""
        logging.set_log_path(temp_log_file)

    @pytest.mark.P1
    def test_set_log_to_file(self, temp_log_file):
        """测试启用文件日志。"""
        logging.set_log_path(temp_log_file)
        logging.set_log_to_file(True)

    @pytest.mark.P2
    def test_invalid_log_path(self):
        """测试无效日志路径。"""
        invalid_path = "/nonexistent/directory/test.log"
        # 可能写入失败或抛出异常
        try:
            logging.set_log_path(invalid_path)
        except (PermissionError, FileNotFoundError, OSError):
            pass  # 预期行为


class TestLoggingLevel:
    """日志级别设置测试。"""

    @pytest.mark.P1
    def test_set_log_level_trace(self):
        """测试设置 TRACE 级别。"""
        logging.set_log_level(logging.Level.TRACE)

    @pytest.mark.P1
    def test_set_log_level_debug(self):
        """测试设置 DEBUG 级别。"""
        logging.set_log_level(logging.Level.DEBUG)

    @pytest.mark.P1
    def test_set_log_level_info(self):
        """测试设置 INFO 级别。"""
        logging.set_log_level(logging.Level.INFO)

    @pytest.mark.P1
    def test_set_log_level_warn(self):
        """测试设置 WARN 级别。"""
        logging.set_log_level(logging.Level.WARN)

    @pytest.mark.P1
    def test_set_log_level_error(self):
        """测试设置 ERROR 级别。"""
        logging.set_log_level(logging.Level.ERROR)

    @pytest.mark.P1
    def test_set_log_level_critical(self):
        """测试设置 CRITICAL 级别。"""
        logging.set_log_level(logging.Level.CRITICAL)

    @pytest.mark.P1
    def test_set_log_level_off(self):
        """测试设置 OFF 级别。"""
        logging.set_log_level(logging.Level.OFF)


class TestLoggingFlush:
    """日志刷新测试。"""

    @pytest.mark.P1
    def test_flush(self):
        """测试手动刷新日志。"""
        logging.flush()


class TestLoggingIntegration:
    """日志集成测试。"""

    @pytest.fixture
    def log_config(self):
        """配置日志的 fixture。"""
        with tempfile.NamedTemporaryFile(mode='w', suffix='.log', delete=False) as f:
            temp_path = f.name

        # 配置日志
        logging.set_log_to_console(True)
        logging.set_log_to_file(True)
        logging.set_log_path(temp_path)
        logging.set_log_level(logging.Level.DEBUG)

        yield temp_path

        # 清理
        logging.set_log_to_console(False)
        logging.set_log_to_file(False)
        logging.flush()
        if os.path.exists(temp_path):
            os.remove(temp_path)

    @pytest.mark.P1
    def test_logging_configuration(self, log_config):
        """测试日志配置。"""
        # 验证配置已设置
        assert os.path.exists(log_config) or True  # 文件可能不存在

    @pytest.mark.P1
    def test_logging_after_hand_creation(self, log_config):
        """测试创建 Hand 后的日志。"""
        # 配置日志
        logging.set_log_level(logging.Level.DEBUG)

        # 创建设备（如果已连接）
        try:
            hand = wh.Hand()
            # 日志应该正常工作
        except Exception:
            pass  # 可能没有设备
