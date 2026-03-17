"""Tests for Hand Zenoh Bridge."""

import json
import numpy as np
from unittest.mock import MagicMock

from bridge.python.hand_zenoh_bridge import (
    build_capability,
    sanitize_sn,
    HandBridge,
    RESOURCE_DEFS,
)


def test_sanitize_sn_with_dots():
    assert sanitize_sn("HAND.001.0000") == "HAND_001_0000"


def test_sanitize_sn_without_dots():
    assert sanitize_sn("HAND_TEST") == "HAND_TEST"


def test_capability_json_structure():
    cap = json.loads(build_capability("HAND_TEST_001"))

    assert cap["serial_number"] == "HAND_TEST_001"
    assert cap["device_proto"] == "custom"
    assert cap["device_id"] == 0
    assert isinstance(cap["resources"], list)
    assert len(cap["resources"]) > 0

    for r in cap["resources"]:
        assert "path" in r
        assert r["schema_id"] == 0
        assert r["serde_format"] == "json"
        assert "json_schema" in r
        assert "title" in r["json_schema"]


def test_capability_has_all_resources():
    cap = json.loads(build_capability("TEST"))
    paths = {r["path"] for r in cap["resources"]}

    assert "input_voltage" in paths
    assert "joint/actual_position" in paths
    assert "joint/actual_effort" in paths
    assert "joint/control_mode" in paths
    assert "joint/enabled" in paths
    assert "joint/target_position" in paths
    assert "joint/effort_limit" in paths
    assert "joint/temperature" in paths


def test_capability_access_flags():
    cap = json.loads(build_capability("TEST"))
    by_path = {r["path"]: r for r in cap["resources"]}

    # input_voltage: GET only
    assert by_path["input_voltage"]["can_get"] is True
    assert by_path["input_voltage"]["can_set"] is False
    assert by_path["input_voltage"]["can_sub"] is False

    # joint/actual_position: GET + SUB
    assert by_path["joint/actual_position"]["can_get"] is True
    assert by_path["joint/actual_position"]["can_sub"] is True

    # joint/enabled: SET only
    assert by_path["joint/enabled"]["can_get"] is False
    assert by_path["joint/enabled"]["can_set"] is True

    # joint/effort_limit: GET + SET
    assert by_path["joint/effort_limit"]["can_get"] is True
    assert by_path["joint/effort_limit"]["can_set"] is True

    # joint/actual_effort: GET + SUB
    assert by_path["joint/actual_effort"]["can_get"] is True
    assert by_path["joint/actual_effort"]["can_sub"] is True

    # joint/control_mode: SET only
    assert by_path["joint/control_mode"]["can_get"] is False
    assert by_path["joint/control_mode"]["can_set"] is True


def test_read_resource_scalar():
    hand = MagicMock()
    hand.read_input_voltage.return_value = 12.5
    bridge = HandBridge(hand, "TEST")
    assert bridge._read_resource("input_voltage") == 12.5
    hand.read_input_voltage.assert_called_once()


def test_read_resource_temperature():
    hand = MagicMock()
    hand.read_temperature.return_value = 45.3
    bridge = HandBridge(hand, "TEST")
    assert bridge._read_resource("temperature") == 45.3


def test_read_resource_array():
    hand = MagicMock()
    hand.read_joint_actual_position.return_value = np.zeros((5, 4))
    bridge = HandBridge(hand, "TEST")
    val = bridge._read_resource("joint/actual_position")
    assert isinstance(val, list)
    assert len(val) == 5
    assert len(val[0]) == 4


def test_write_resource_enabled():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST")
    bridge._write_resource("joint/enabled", [[True] * 4] * 5)
    hand.write_joint_enabled.assert_called_once()
    arg = hand.write_joint_enabled.call_args[0][0]
    assert arg.dtype == bool
    assert arg.shape == (5, 4)


def test_write_resource_target_position():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST")
    bridge._write_resource("joint/target_position", [[0.5] * 4] * 5)
    # target_position now updates _rt_target atomically (realtime controller path)
    # instead of calling hand.write_joint_target_position via SDO
    hand.write_joint_target_position.assert_not_called()
    np.testing.assert_array_almost_equal(bridge._rt_target, np.full((5, 4), 0.5))


def test_write_resource_control_mode():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST")
    bridge._write_resource("joint/control_mode", [[1] * 4] * 5)
    hand.write_joint_control_mode.assert_called_once()
    arg = hand.write_joint_control_mode.call_args[0][0]
    assert arg.dtype == np.int32
    assert arg.shape == (5, 4)


def test_read_resource_actual_effort_from_controller():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST")
    # Simulate realtime controller being active
    mock_ctrl = MagicMock()
    mock_ctrl.get_joint_actual_effort.return_value = np.ones((5, 4)) * 0.5
    bridge._controller = mock_ctrl
    val = bridge._read_resource("joint/actual_effort")
    assert isinstance(val, list)
    assert len(val) == 5
    assert val[0][0] == 0.5
    mock_ctrl.get_joint_actual_effort.assert_called_once()


def test_write_resource_effort_limit():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST")
    bridge._write_resource("joint/effort_limit", [[1.0] * 4] * 5)
    hand.write_joint_effort_limit.assert_called_once()


def test_read_unknown_resource_raises():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST")
    try:
        bridge._read_resource("nonexistent")
        assert False, "Should have raised ValueError"
    except ValueError as e:
        assert "nonexistent" in str(e)


def test_write_unknown_resource_raises():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST")
    try:
        bridge._write_resource("nonexistent", 42)
        assert False, "Should have raised ValueError"
    except ValueError as e:
        assert "nonexistent" in str(e)


def test_key_generation():
    hand = MagicMock()
    bridge = HandBridge(hand, "HAND.001")
    assert bridge._key("@alive") == "wuji/HAND_001/@alive"
    assert bridge._key("@capability") == "wuji/HAND_001/@capability"
    assert bridge._key("joint/actual_position") == "wuji/HAND_001/joint/actual_position"


def test_resource_defs_count():
    assert len(RESOURCE_DEFS) == 16
