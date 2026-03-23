"""Tests for Hand Zenoh Bridge."""

import json
import sys
import time
import numpy as np
import pytest
from unittest.mock import MagicMock, patch

# Mock zenoh if not available (e.g., CI without Rust toolchain)
if "zenoh" not in sys.modules:
    sys.modules["zenoh"] = MagicMock()

import bridge.python.hand_zenoh_bridge as hand_zenoh_bridge_module
from bridge.python.hand_zenoh_bridge import (
    build_capability,
    sanitize_sn,
    get_timestamp_us,
    wrap_with_timestamp,
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
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    assert bridge._read_resource("input_voltage") == 12.5
    hand.read_input_voltage.assert_called_once()


def test_read_resource_temperature():
    hand = MagicMock()
    hand.read_temperature.return_value = 45.3
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    assert bridge._read_resource("temperature") == 45.3


def test_read_resource_array():
    hand = MagicMock()
    hand.read_joint_actual_position.return_value = np.zeros((5, 4))
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    val = bridge._read_resource("joint/actual_position")
    assert isinstance(val, list)
    assert len(val) == 5
    assert len(val[0]) == 4


def test_write_resource_enabled():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    bridge._write_resource("joint/enabled", [[True] * 4] * 5)
    hand.write_joint_enabled.assert_called_once()
    arg = hand.write_joint_enabled.call_args[0][0]
    assert arg.dtype == bool
    assert arg.shape == (5, 4)


def test_write_resource_target_position():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    bridge._write_resource("joint/target_position", [[0.5] * 4] * 5)
    # target_position now updates _rt_target atomically (realtime controller path)
    # instead of calling hand.write_joint_target_position via SDO
    hand.write_joint_target_position.assert_not_called()
    np.testing.assert_array_almost_equal(bridge._rt_target, np.full((5, 4), 0.5))


def test_write_resource_control_mode():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    bridge._write_resource("joint/control_mode", [[1] * 4] * 5)
    hand.write_joint_control_mode.assert_called_once()
    arg = hand.write_joint_control_mode.call_args[0][0]
    assert arg.dtype == np.int32
    assert arg.shape == (5, 4)


def test_read_resource_actual_effort_from_controller():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    # Simulate realtime controller being active
    mock_ctrl = MagicMock()
    mock_ctrl.get_joint_actual_effort.return_value = np.ones((5, 4)) * 0.5
    bridge._controller = mock_ctrl
    val = bridge._read_resource("joint/actual_effort")
    assert isinstance(val, list)
    assert len(val) == 5
    assert val[0][0] == 0.5
    mock_ctrl.get_joint_actual_effort.assert_called_once()


def test_read_resource_actual_effort_without_controller_returns_zero_snapshot():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)

    val = bridge._read_resource("joint/actual_effort")

    assert val == [[0.0] * 4 for _ in range(5)]


def test_write_resource_effort_limit():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    bridge._write_resource("joint/effort_limit", [[1.0] * 4] * 5)
    hand.write_joint_effort_limit.assert_called_once()


def test_read_unknown_resource_raises():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    with pytest.raises(ValueError, match="nonexistent"):
        bridge._read_resource("nonexistent")


def test_write_unknown_resource_raises():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    with pytest.raises(ValueError, match="nonexistent"):
        bridge._write_resource("nonexistent", 42)


def test_key_generation():
    hand = MagicMock()
    bridge = HandBridge(hand, "HAND.001", pub_rate=100.0)
    assert bridge._key("@alive") == "wuji/HAND_001/@alive"
    assert bridge._key("@capability") == "wuji/HAND_001/@capability"
    assert bridge._key("joint/actual_position") == "wuji/HAND_001/joint/actual_position"


def test_resource_defs_count():
    paths = [resource["path"] for resource in RESOURCE_DEFS]
    assert len(RESOURCE_DEFS) >= 10
    assert len(paths) == len(set(paths))


# ---------------------------------------------------------------------------
# Timestamp tests
# ---------------------------------------------------------------------------

def test_get_timestamp_us_returns_microseconds():
    ts = get_timestamp_us()
    # Should be in microseconds (roughly current epoch in us)
    assert ts > 1_700_000_000_000_000  # after 2023
    assert ts < 2_000_000_000_000_000  # before 2033
    # Should be monotonically increasing
    ts2 = get_timestamp_us()
    assert ts2 >= ts


def test_wrap_with_timestamp_structure():
    value = [[1.0, 2.0], [3.0, 4.0]]
    envelope = wrap_with_timestamp(value)
    assert "timestamp_us" in envelope
    assert "data" in envelope
    assert envelope["data"] == value
    assert isinstance(envelope["timestamp_us"], int)
    assert envelope["timestamp_us"] > 0


def test_wrap_with_timestamp_custom_ts():
    value = 42
    envelope = wrap_with_timestamp(value, timestamp_us=1234567890)
    assert envelope["timestamp_us"] == 1234567890
    assert envelope["data"] == 42


def test_wrap_with_timestamp_json_serializable():
    value = [[1.0, 2.0, 3.0, 4.0]] * 5
    envelope = wrap_with_timestamp(value)
    serialized = json.dumps(envelope)
    deserialized = json.loads(serialized)
    assert deserialized["timestamp_us"] == envelope["timestamp_us"]
    assert deserialized["data"] == value


def test_capability_sub_resources_have_timestamp_schema():
    cap = json.loads(build_capability("TEST"))
    by_path = {r["path"]: r for r in cap["resources"]}

    # SUB resources should have timestamped envelope schema
    pos = by_path["joint/actual_position"]
    assert pos["can_sub"] is True
    schema = pos["json_schema"]
    assert schema["type"] == "object"
    assert "timestamp_us" in schema["properties"]
    assert "data" in schema["properties"]
    assert schema["properties"]["timestamp_us"]["type"] == "integer"

    # Non-SUB resources should NOT have envelope schema
    temp = by_path["joint/temperature"]
    assert temp["can_sub"] is False
    assert temp["json_schema"]["type"] == "array"  # original schema unchanged


# ---------------------------------------------------------------------------
# Control TTL tests
# ---------------------------------------------------------------------------

def test_control_acquire_sets_owner():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    bridge.session = MagicMock()
    # Simulate acquire
    with bridge._control_lock:
        bridge._control_owner = "zid_123"
    assert bridge._control_owner == "zid_123"


def test_control_release_clears_owner():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    bridge.session = MagicMock()
    bridge._control_owner = "zid_123"
    with bridge._control_lock:
        bridge._control_owner = None
    assert bridge._control_owner is None


def test_control_owner_watcher_key():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    key = bridge._control_owner_key("zid_abc123")
    assert key == "wuji/TEST/@control_owner/zid_abc123"


def test_stop_owner_watcher_cleans_up():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    mock_watcher = MagicMock()
    bridge._control_owner_watcher = mock_watcher
    bridge._stop_owner_watcher()
    mock_watcher.undeclare.assert_called_once()
    assert bridge._control_owner_watcher is None


def test_stop_owner_watcher_noop_when_none():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    bridge._control_owner_watcher = None
    bridge._stop_owner_watcher()  # should not raise
    assert bridge._control_owner_watcher is None


def test_stop_owner_watcher_logs_debug_on_undeclare_error():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    mock_watcher = MagicMock()
    mock_watcher.undeclare.side_effect = RuntimeError("boom")
    bridge._control_owner_watcher = mock_watcher

    with patch.object(hand_zenoh_bridge_module.logger, "debug") as debug_mock:
        bridge._stop_owner_watcher()

    debug_mock.assert_called_once_with("Failed to undeclare owner watcher: boom")
    assert bridge._control_owner_watcher is None


def test_start_owner_watcher_raises_when_subscriber_creation_returns_none():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    bridge.session = MagicMock()
    bridge.session.liveliness.return_value.declare_subscriber.return_value = None

    with pytest.raises(RuntimeError, match="declare_subscriber returned None"):
        bridge._start_owner_watcher("zid_123")

    assert bridge._control_owner_watcher is None


def test_start_owner_watcher_releases_owner_on_delete_sample(monkeypatch):
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    bridge.session = MagicMock()
    delete_kind = object()
    monkeypatch.setattr(hand_zenoh_bridge_module.zenoh, "SampleKind", MagicMock(DELETE=delete_kind))

    captured = {}
    watcher = MagicMock()

    def declare_subscriber(_key, callback):
        captured["callback"] = callback
        return watcher

    bridge.session.liveliness.return_value.declare_subscriber.side_effect = declare_subscriber
    bridge._control_owner = "zid_123"
    bridge._start_owner_watcher("zid_123")

    sample = MagicMock()
    sample.kind = delete_kind
    captured["callback"](sample)

    assert bridge._control_owner is None
    watcher.undeclare.assert_called_once()
    assert bridge._control_owner_watcher is None


def test_handle_control_acquire_rolls_back_when_watcher_start_fails():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    bridge._start_owner_watcher = MagicMock(side_effect=RuntimeError("boom"))
    query = MagicMock()
    query.payload = b"acquire:zid_123"
    query.attachment = b"zid_123"

    bridge._handle_control(query)

    assert bridge._control_owner is None
    query.reply.assert_not_called()
    query.reply_err.assert_called_once_with(b"boom")


def test_handle_control_rejects_attachment_mismatch():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    query = MagicMock()
    query.payload = b"acquire:zid_123"
    query.attachment = b"zid_other"

    bridge._handle_control(query)

    assert bridge._control_owner is None
    query.reply.assert_not_called()
    query.reply_err.assert_called_once_with(b"identity_mismatch")


def test_handle_resource_query_get_replies_with_raw_json():
    hand = MagicMock()
    hand.read_input_voltage.return_value = 12.5
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    query = MagicMock()
    query.payload = None
    query.attachment = None
    resource_def = {"path": "input_voltage", "can_get": True, "can_set": False}

    bridge._handle_resource_query(query, resource_def)

    reply_key, reply_payload = query.reply.call_args[0]
    assert reply_key == "wuji/TEST/input_voltage"
    assert json.loads(reply_payload.decode("utf-8")) == 12.5


def test_handle_resource_query_set_requires_requester_attachment():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    bridge._control_owner = "zid_123"
    query = MagicMock()
    query.payload = json.dumps([[True] * 4 for _ in range(5)]).encode("utf-8")
    query.attachment = None
    resource_def = {"path": "joint/enabled", "can_get": False, "can_set": True}

    bridge._handle_resource_query(query, resource_def)

    hand.write_joint_enabled.assert_not_called()
    query.reply.assert_not_called()
    query.reply_err.assert_called_once_with(b"missing requester id")


def test_handle_resource_query_set_rejects_non_owner_requester():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    bridge._control_owner = "zid_owner"
    query = MagicMock()
    query.payload = json.dumps([[True] * 4 for _ in range(5)]).encode("utf-8")
    query.attachment = b"zid_other"
    resource_def = {"path": "joint/enabled", "can_get": False, "can_set": True}

    bridge._handle_resource_query(query, resource_def)

    hand.write_joint_enabled.assert_not_called()
    query.reply.assert_not_called()
    query.reply_err.assert_called_once_with(b"not control owner")


def test_handle_target_position_put_updates_rt_target():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    bridge._control_owner = "zid_123"
    sample = MagicMock()
    sample.payload = json.dumps([[0.25] * 4 for _ in range(5)]).encode("utf-8")
    sample.attachment = b"zid_123"

    bridge._handle_target_position_put(sample)

    np.testing.assert_array_almost_equal(bridge._rt_target, np.full((5, 4), 0.25))


def test_handle_target_position_put_ignores_without_control_owner():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    sample = MagicMock()
    sample.payload = json.dumps([[0.25] * 4 for _ in range(5)]).encode("utf-8")
    sample.attachment = b"zid_123"

    with patch.object(hand_zenoh_bridge_module.logger, "warning") as warning_mock:
        bridge._handle_target_position_put(sample)

    np.testing.assert_array_almost_equal(bridge._rt_target, np.zeros((5, 4)))
    warning_mock.assert_called_once_with("Ignoring target_position PUT without control owner")


def test_handle_target_position_put_logs_warning_for_invalid_shape():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    bridge._control_owner = "zid_123"
    sample = MagicMock()
    sample.payload = json.dumps([[1.0] * 4]).encode("utf-8")
    sample.attachment = b"zid_123"

    with patch.object(hand_zenoh_bridge_module.logger, "warning") as warning_mock:
        bridge._handle_target_position_put(sample)

    warning_mock.assert_called_once_with(
        "Invalid target_position PUT ignored: target_position must be 5x4 array, got shape (1, 4)"
    )


def test_handle_target_position_put_rejects_non_owner_attachment():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    bridge._control_owner = "zid_owner"
    sample = MagicMock()
    sample.payload = json.dumps([[0.25] * 4 for _ in range(5)]).encode("utf-8")
    sample.attachment = b"zid_other"

    with patch.object(hand_zenoh_bridge_module.logger, "warning") as warning_mock:
        bridge._handle_target_position_put(sample)

    np.testing.assert_array_almost_equal(bridge._rt_target, np.zeros((5, 4)))
    warning_mock.assert_called_once_with(
        "Ignoring target_position PUT from non-owner requester %s (owner=%s)",
        "zid_other",
        "zid_owner",
    )


def test_bridge_has_control_lock():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    assert hasattr(bridge, '_control_lock')
    assert hasattr(bridge, '_control_owner_watcher')


def test_start_cleans_up_partial_state_on_failure(monkeypatch):
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST", pub_rate=100.0)
    mock_session = MagicMock()
    mock_session.zid.return_value = "zid_123"
    mock_token = MagicMock()
    mock_capability_queryable = MagicMock()
    mock_session.liveliness.return_value.declare_token.return_value = mock_token
    mock_session.declare_queryable.side_effect = [
        mock_capability_queryable,
        RuntimeError("boom"),
    ]

    monkeypatch.setattr(
        hand_zenoh_bridge_module.zenoh,
        "Config",
        MagicMock(return_value=MagicMock()),
    )
    monkeypatch.setattr(
        hand_zenoh_bridge_module.zenoh,
        "open",
        MagicMock(return_value=mock_session),
    )

    bridge._start_realtime_controller = MagicMock()
    bridge._stop_realtime_controller = MagicMock()

    with pytest.raises(RuntimeError, match="boom"):
        bridge.start()

    bridge._stop_realtime_controller.assert_called_once()
    mock_capability_queryable.undeclare.assert_called_once()
    mock_token.undeclare.assert_called_once()
    mock_session.close.assert_called_once()
    assert bridge._running is False
    assert bridge.session is None
    assert bridge._alive_token is None
    assert bridge._queryables == []
    assert bridge._subscribers == []
    assert bridge._publishers == {}
    assert bridge._threads == []
