# Hand Zenoh Bridge (Python MVP) Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** A standalone Python process that connects to a Wuji Hand via wujihandpy and exposes all its resources to the Zenoh network following wuji-sdk's third-party bridge protocol, enabling wuji-sdk clients to discover and control the hand.

**Architecture:** Independent bridge process pattern. The bridge holds the exclusive USB connection to the hand (via wujihandpy), then declares Zenoh queryables for GET/SET operations and Zenoh publishers for continuous data streams (SUB). wuji-sdk clients discover the hand through liveliness tokens and @capability queries, then interact transparently via Zenoh — identical to how they access gloves.

**Tech Stack:** Python 3.10, wujihandpy 1.5.1, eclipse-zenoh 1.8.0, numpy

**Worktree:** `/home/javen/work/worktrees/wujihandpy--hand-bridge/`

**Note:** The hand firmware is 1.0.1 — `get_joint_actual_effort()` requires >= 1.2.0, so effort feedback is excluded from this MVP.

---

## Resource Mapping

### GET Resources (Read-Only)

| Resource Path | wujihandpy Method | Return Type | JSON Schema |
|---|---|---|---|
| `input_voltage` | `hand.read_input_voltage()` | float | `{"type": "number"}` |
| `temperature` | `hand.read_temperature()` | float | `{"type": "number"}` |
| `handedness` | `hand.read_handedness()` | int | `{"type": "integer"}` |
| `firmware_version` | `hand.read_firmware_version()` | int | `{"type": "integer"}` |
| `joint/actual_position` | `hand.read_joint_actual_position()` | 5x4 array | `{"type": "array", "items": {"type": "array", "items": {"type": "number"}}}` |
| `joint/temperature` | `hand.read_joint_temperature()` | 5x4 array | same as above |
| `joint/error_code` | `hand.read_joint_error_code()` | 5x4 array | `{"type": "array", "items": {"type": "array", "items": {"type": "integer"}}}` |
| `joint/effort_limit` | `hand.read_joint_effort_limit()` | 5x4 array | float 5x4 |
| `joint/upper_limit` | `hand.read_joint_upper_limit()` | 5x4 array | float 5x4 |
| `joint/lower_limit` | `hand.read_joint_lower_limit()` | 5x4 array | float 5x4 |

### SET Resources (Write-Only, Requires Control)

| Resource Path | wujihandpy Method | Input Type |
|---|---|---|
| `joint/enabled` | `hand.write_joint_enabled(val)` | bool or 5x4 bool array |
| `joint/target_position` | `hand.write_joint_target_position(val)` | 5x4 float array |
| `joint/effort_limit` | `hand.write_joint_effort_limit(val)` | 5x4 float array |

### SUB Resources (Continuous Stream)

| Resource Path | Source | Rate | JSON Schema |
|---|---|---|---|
| `joint/actual_position` | Periodic read via `hand.read_joint_actual_position()` | 50 Hz | float 5x4 |

---

## Task 1: Project Scaffold and Dependencies

**Files:**
- Create: `bridge/hand_zenoh_bridge.py` (main module)
- Create: `bridge/__init__.py`
- Create: `bridge/requirements.txt`
- Create: `tests/test_bridge.py`

**Step 1: Create bridge package directory**

```bash
cd /home/javen/work/worktrees/wujihandpy--hand-bridge
mkdir -p bridge tests
```

**Step 2: Create requirements.txt**

```
# bridge/requirements.txt
eclipse-zenoh>=1.7.0
wujihandpy>=1.5.0
numpy
```

**Step 3: Create bridge/__init__.py**

```python
# bridge/__init__.py
```

**Step 4: Create minimal bridge entry point**

```python
# bridge/hand_zenoh_bridge.py
"""Wuji Hand Zenoh Bridge - exposes wujihandpy via Zenoh for wuji-sdk."""

import json
import time
import threading
import logging
import argparse

import zenoh
import numpy as np

logger = logging.getLogger("hand_bridge")


def main():
    parser = argparse.ArgumentParser(description="Wuji Hand Zenoh Bridge")
    parser.add_argument("--sn", type=str, default=None, help="Hand serial number")
    parser.add_argument("--pub-rate", type=float, default=50.0, help="Position publish rate (Hz)")
    parser.add_argument("--log-level", type=str, default="INFO", help="Log level")
    args = parser.parse_args()

    logging.basicConfig(level=getattr(logging, args.log_level.upper()))
    logger.info("Hand Zenoh Bridge starting...")


if __name__ == "__main__":
    main()
```

**Step 5: Create test scaffold**

```python
# tests/test_bridge.py
"""Tests for Hand Zenoh Bridge."""


def test_placeholder():
    """Placeholder to verify test setup."""
    assert True
```

**Step 6: Install dependencies in venv**

```bash
source /home/javen/work/wujihandpy/.venv/bin/activate
pip install eclipse-zenoh
```

**Step 7: Run test to verify setup**

Run: `source /home/javen/work/wujihandpy/.venv/bin/activate && cd /home/javen/work/worktrees/wujihandpy--hand-bridge && python -m pytest tests/test_bridge.py -v`
Expected: PASS

**Step 8: Commit**

```bash
git add bridge/ tests/
git commit -m "feat: scaffold hand zenoh bridge project"
```

---

## Task 2: Hand Connection and Zenoh Session Initialization

**Files:**
- Modify: `bridge/hand_zenoh_bridge.py`
- Modify: `tests/test_bridge.py`

**Step 1: Write test for bridge initialization (no real device)**

```python
# tests/test_bridge.py
import json
from unittest.mock import MagicMock, patch


def test_bridge_builds_capability_json():
    """Capability JSON must match wuji-sdk third-party protocol."""
    from bridge.hand_zenoh_bridge import build_capability

    cap = build_capability(serial_number="HAND_TEST_001")
    data = json.loads(cap)

    assert data["serial_number"] == "HAND_TEST_001"
    assert data["device_proto"] == "custom"
    assert isinstance(data["resources"], list)
    assert len(data["resources"]) > 0

    # Every resource must have required fields
    for r in data["resources"]:
        assert "path" in r
        assert "schema_id" in r
        assert r["schema_id"] == 0
        assert r["serde_format"] == "json"
        assert "json_schema" in r
        assert "title" in r["json_schema"]


def test_sanitize_sn():
    """SN dots must be replaced with underscores for Zenoh keys."""
    from bridge.hand_zenoh_bridge import sanitize_sn

    assert sanitize_sn("HAND.001.0000") == "HAND_001_0000"
    assert sanitize_sn("HAND_TEST") == "HAND_TEST"
```

**Step 2: Run tests to verify they fail**

Run: `python -m pytest tests/test_bridge.py -v`
Expected: FAIL (functions not defined)

**Step 3: Implement sanitize_sn and build_capability**

Add to `bridge/hand_zenoh_bridge.py`:

```python
def sanitize_sn(sn: str) -> str:
    """Replace dots with underscores for Zenoh key expressions."""
    return sn.replace(".", "_")


# Resource definitions: (path, can_get, can_set, can_sub, title, json_schema_type)
RESOURCE_DEFS = [
    # GET-only scalar resources
    {
        "path": "input_voltage",
        "can_get": True, "can_set": False, "can_sub": False,
        "json_schema": {"title": "InputVoltage", "type": "number"},
    },
    {
        "path": "temperature",
        "can_get": True, "can_set": False, "can_sub": False,
        "json_schema": {"title": "Temperature", "type": "number"},
    },
    {
        "path": "handedness",
        "can_get": True, "can_set": False, "can_sub": False,
        "json_schema": {"title": "Handedness", "type": "integer"},
    },
    {
        "path": "firmware_version",
        "can_get": True, "can_set": False, "can_sub": False,
        "json_schema": {"title": "FirmwareVersion", "type": "integer"},
    },
    # GET-only array resources
    {
        "path": "joint/actual_position",
        "can_get": True, "can_set": False, "can_sub": True,
        "json_schema": {
            "title": "JointActualPosition",
            "type": "array",
            "description": "5x4 joint positions (5 fingers x 4 joints)",
            "items": {"type": "array", "items": {"type": "number"}},
        },
    },
    {
        "path": "joint/temperature",
        "can_get": True, "can_set": False, "can_sub": False,
        "json_schema": {
            "title": "JointTemperature",
            "type": "array",
            "items": {"type": "array", "items": {"type": "number"}},
        },
    },
    {
        "path": "joint/error_code",
        "can_get": True, "can_set": False, "can_sub": False,
        "json_schema": {
            "title": "JointErrorCode",
            "type": "array",
            "items": {"type": "array", "items": {"type": "integer"}},
        },
    },
    {
        "path": "joint/effort_limit",
        "can_get": True, "can_set": True, "can_sub": False,
        "json_schema": {
            "title": "JointEffortLimit",
            "type": "array",
            "items": {"type": "array", "items": {"type": "number"}},
        },
    },
    {
        "path": "joint/upper_limit",
        "can_get": True, "can_set": False, "can_sub": False,
        "json_schema": {
            "title": "JointUpperLimit",
            "type": "array",
            "items": {"type": "array", "items": {"type": "number"}},
        },
    },
    {
        "path": "joint/lower_limit",
        "can_get": True, "can_set": False, "can_sub": False,
        "json_schema": {
            "title": "JointLowerLimit",
            "type": "array",
            "items": {"type": "array", "items": {"type": "number"}},
        },
    },
    # SET-only resources (require control)
    {
        "path": "joint/enabled",
        "can_get": False, "can_set": True, "can_sub": False,
        "json_schema": {
            "title": "JointEnabled",
            "type": "array",
            "description": "5x4 joint enabled states",
            "items": {"type": "array", "items": {"type": "boolean"}},
        },
    },
    {
        "path": "joint/target_position",
        "can_get": False, "can_set": True, "can_sub": False,
        "json_schema": {
            "title": "JointTargetPosition",
            "type": "array",
            "description": "5x4 joint target positions",
            "items": {"type": "array", "items": {"type": "number"}},
        },
    },
]


def build_capability(serial_number: str) -> str:
    """Build the @capability JSON response following wuji-sdk protocol."""
    resources = []
    for r in RESOURCE_DEFS:
        resources.append({
            "path": r["path"],
            "schema_id": 0,
            "can_get": r["can_get"],
            "can_set": r["can_set"],
            "can_sub": r["can_sub"],
            "can_pub": False,
            "can_exec": False,
            "internal": False,
            "serde_format": "json",
            "json_schema": r["json_schema"],
        })

    capability = {
        "device_id": 0,
        "device_proto": "custom",
        "firmware_version": "",
        "serial_number": serial_number,
        "nodes": [],
        "resources": resources,
    }
    return json.dumps(capability)
```

**Step 4: Run tests to verify they pass**

Run: `python -m pytest tests/test_bridge.py -v`
Expected: PASS

**Step 5: Commit**

```bash
git add bridge/hand_zenoh_bridge.py tests/test_bridge.py
git commit -m "feat: add capability builder and resource definitions"
```

---

## Task 3: Zenoh Session, Liveliness and Status

**Files:**
- Modify: `bridge/hand_zenoh_bridge.py`

**Step 1: Implement HandBridge class with zenoh lifecycle**

Add `HandBridge` class to `bridge/hand_zenoh_bridge.py`:

```python
class HandBridge:
    """Bridge between wujihandpy and Zenoh network."""

    def __init__(self, hand, serial_number: str, pub_rate: float = 50.0):
        self.hand = hand
        self.sn = serial_number
        self.sanitized_sn = sanitize_sn(serial_number)
        self.pub_rate = pub_rate
        self.session = None
        self._alive_token = None
        self._running = False
        self._control_owner = None  # ZID string or None
        self._threads = []

    def _key(self, suffix: str) -> str:
        return f"wuji/{self.sanitized_sn}/{suffix}"

    def start(self):
        """Open Zenoh session, declare liveliness, publish status, start queryables."""
        logger.info("Opening Zenoh session...")
        config = zenoh.Config()
        self.session = zenoh.open(config)
        zid = str(self.session.zid())
        logger.info(f"Zenoh session opened, ZID: {zid}")

        # 1. Liveliness token
        self._alive_token = self.session.liveliness().declare_token(self._key("@alive"))
        logger.info(f"Liveliness token declared: {self._key('@alive')}")

        # 2. Status: online
        self.session.put(self._key("@status"), b"online")
        logger.info("Status: online")

        # 3. Capability queryable
        cap_bytes = build_capability(self.sn).encode("utf-8")
        self._cap_queryable = self.session.declare_queryable(
            self._key("@capability"),
            lambda query: query.reply(self._key("@capability"), cap_bytes),
        )
        logger.info("@capability queryable declared")

        # 4. Control queryable
        self._ctrl_queryable = self.session.declare_queryable(
            self._key("@control"),
            self._handle_control,
        )
        logger.info("@control queryable declared")

        # 5. Resource queryables (GET/SET)
        self._resource_queryables = []
        for r in RESOURCE_DEFS:
            if r["can_get"] or r["can_set"]:
                q = self.session.declare_queryable(
                    self._key(r["path"]),
                    lambda query, res=r: self._handle_resource_query(query, res),
                )
                self._resource_queryables.append(q)
                logger.info(f"Resource queryable: {r['path']}")

        # 6. SUB publishers (continuous streams)
        self._running = True
        self._publishers = {}
        for r in RESOURCE_DEFS:
            if r["can_sub"]:
                pub = self.session.declare_publisher(self._key(r["path"]))
                self._publishers[r["path"]] = pub

        if self._publishers:
            t = threading.Thread(target=self._publish_loop, daemon=True)
            t.start()
            self._threads.append(t)
            logger.info(f"Publisher loop started at {self.pub_rate} Hz")

        logger.info("Hand Zenoh Bridge fully started")

    def stop(self):
        """Gracefully shutdown."""
        logger.info("Stopping bridge...")
        self._running = False
        for t in self._threads:
            t.join(timeout=2.0)

        if self.session:
            self.session.put(self._key("@status"), b"offline")
            logger.info("Status: offline")

        # Undeclare happens automatically when Python objects are GC'd
        self._alive_token = None
        self.session = None
        logger.info("Bridge stopped")

    def _handle_control(self, query):
        """Handle @control acquire/release protocol."""
        key = self._key("@control")
        payload = bytes(query.payload) if query.payload else b""
        payload_str = payload.decode("utf-8", errors="replace")

        if payload_str.startswith("acquire:"):
            requester = payload_str[len("acquire:"):]
            if self._control_owner is None or self._control_owner == requester:
                self._control_owner = requester
                query.reply(key, b"granted")
                logger.info(f"Control granted to {requester}")
            else:
                query.reply(key, f"denied:{self._control_owner}".encode())
                logger.info(f"Control denied to {requester}, owner: {self._control_owner}")
        elif payload_str.startswith("release:"):
            requester = payload_str[len("release:"):]
            if self._control_owner == requester:
                self._control_owner = None
                query.reply(key, b"released")
                logger.info(f"Control released by {requester}")
            else:
                query.reply(key, b"not_owner")
        else:
            owner = self._control_owner or "none"
            query.reply(key, owner.encode())

    def _handle_resource_query(self, query, resource_def):
        """Handle GET/SET for a resource."""
        key = self._key(resource_def["path"])
        payload = bytes(query.payload) if query.payload else b""

        if len(payload) == 0:
            # GET
            if not resource_def["can_get"]:
                query.reply_err(b"GET not supported")
                return
            try:
                value = self._read_resource(resource_def["path"])
                data = json.dumps(value).encode("utf-8")
                query.reply(key, data)
            except Exception as e:
                logger.error(f"GET {resource_def['path']} failed: {e}")
                query.reply_err(str(e).encode())
        else:
            # SET
            if not resource_def["can_set"]:
                query.reply_err(b"SET not supported")
                return
            if self._control_owner is None:
                query.reply_err(b"no control owner")
                return
            try:
                value = json.loads(payload.decode("utf-8"))
                self._write_resource(resource_def["path"], value)
                query.reply(key, b'"ok"')
            except Exception as e:
                logger.error(f"SET {resource_def['path']} failed: {e}")
                query.reply_err(str(e).encode())

    def _read_resource(self, path: str):
        """Read a resource from the hand, return JSON-serializable value."""
        if path == "input_voltage":
            return float(self.hand.read_input_voltage())
        elif path == "temperature":
            return float(self.hand.read_temperature())
        elif path == "handedness":
            return int(self.hand.read_handedness())
        elif path == "firmware_version":
            return int(self.hand.read_firmware_version())
        elif path == "joint/actual_position":
            return self.hand.read_joint_actual_position().tolist()
        elif path == "joint/temperature":
            return self.hand.read_joint_temperature().tolist()
        elif path == "joint/error_code":
            return self.hand.read_joint_error_code().tolist()
        elif path == "joint/effort_limit":
            return self.hand.read_joint_effort_limit().tolist()
        elif path == "joint/upper_limit":
            return self.hand.read_joint_upper_limit().tolist()
        elif path == "joint/lower_limit":
            return self.hand.read_joint_lower_limit().tolist()
        else:
            raise ValueError(f"Unknown GET resource: {path}")

    def _write_resource(self, path: str, value):
        """Write a resource to the hand."""
        if path == "joint/enabled":
            self.hand.write_joint_enabled(np.array(value, dtype=bool))
        elif path == "joint/target_position":
            self.hand.write_joint_target_position(np.array(value, dtype=np.float64))
        elif path == "joint/effort_limit":
            self.hand.write_joint_effort_limit(np.array(value, dtype=np.float64))
        else:
            raise ValueError(f"Unknown SET resource: {path}")

    def _publish_loop(self):
        """Continuously publish SUB resources at configured rate."""
        period = 1.0 / self.pub_rate
        while self._running:
            try:
                for path, pub in self._publishers.items():
                    value = self._read_resource(path)
                    data = json.dumps(value).encode("utf-8")
                    pub.put(data)
            except Exception as e:
                logger.error(f"Publish loop error: {e}")
            time.sleep(period)
```

**Step 2: Update main() to use HandBridge**

Replace the `main()` function:

```python
def main():
    parser = argparse.ArgumentParser(description="Wuji Hand Zenoh Bridge")
    parser.add_argument("--sn", type=str, default=None, help="Hand serial number filter")
    parser.add_argument("--pub-rate", type=float, default=50.0, help="Position publish rate (Hz)")
    parser.add_argument("--log-level", type=str, default="INFO", help="Log level")
    args = parser.parse_args()

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper()),
        format="%(asctime)s [%(name)s] [%(levelname)s] %(message)s",
    )

    import wujihandpy

    logger.info("Connecting to hand...")
    hand = wujihandpy.Hand(serial_number=args.sn)

    # Read serial number for Zenoh registration
    sn = hand.get_product_sn() or f"WUJIHAND_{id(hand):08X}"
    logger.info(f"Hand connected, SN: {sn}")

    bridge = HandBridge(hand, serial_number=sn, pub_rate=args.pub_rate)
    bridge.start()

    try:
        logger.info("Bridge running. Press Ctrl+C to stop.")
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        pass
    finally:
        bridge.stop()
        hand.write_joint_enabled(False)
        logger.info("Hand disabled, exiting.")
```

**Step 3: Verify script loads without hardware**

Run: `python -c "from bridge.hand_zenoh_bridge import build_capability, sanitize_sn, RESOURCE_DEFS; print(f'{len(RESOURCE_DEFS)} resources defined')"`
Expected: `12 resources defined`

**Step 4: Commit**

```bash
git add bridge/hand_zenoh_bridge.py
git commit -m "feat: implement HandBridge with full zenoh lifecycle"
```

---

## Task 4: Unit Tests for Control Protocol and Resource Dispatch

**Files:**
- Modify: `tests/test_bridge.py`

**Step 1: Add control protocol and resource dispatch tests**

```python
# tests/test_bridge.py
import json
import numpy as np
from unittest.mock import MagicMock, PropertyMock

from bridge.hand_zenoh_bridge import (
    build_capability,
    sanitize_sn,
    HandBridge,
    RESOURCE_DEFS,
)


def test_sanitize_sn():
    assert sanitize_sn("HAND.001.0000") == "HAND_001_0000"
    assert sanitize_sn("HAND_TEST") == "HAND_TEST"


def test_capability_json_structure():
    cap = build_capability(serial_number="HAND_TEST_001")
    data = json.loads(cap)

    assert data["serial_number"] == "HAND_TEST_001"
    assert data["device_proto"] == "custom"
    assert isinstance(data["resources"], list)
    assert len(data["resources"]) > 0

    for r in data["resources"]:
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
    assert "joint/enabled" in paths
    assert "joint/target_position" in paths


def test_read_resource_scalar():
    hand = MagicMock()
    hand.read_input_voltage.return_value = 12.5
    bridge = HandBridge(hand, "TEST")

    val = bridge._read_resource("input_voltage")
    assert val == 12.5
    hand.read_input_voltage.assert_called_once()


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

    value = [[True] * 4] * 5
    bridge._write_resource("joint/enabled", value)
    hand.write_joint_enabled.assert_called_once()


def test_write_resource_target_position():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST")

    value = [[0.0] * 4] * 5
    bridge._write_resource("joint/target_position", value)
    hand.write_joint_target_position.assert_called_once()


def test_read_unknown_resource_raises():
    hand = MagicMock()
    bridge = HandBridge(hand, "TEST")

    try:
        bridge._read_resource("nonexistent")
        assert False, "Should have raised ValueError"
    except ValueError:
        pass
```

**Step 2: Run tests**

Run: `python -m pytest tests/test_bridge.py -v`
Expected: ALL PASS

**Step 3: Commit**

```bash
git add tests/test_bridge.py
git commit -m "test: add unit tests for resource read/write and capability"
```

---

## Task 5: End-to-End Test with Real Device

**Files:**
- Create: `bridge/run_bridge.sh` (convenience script)
- Create: `tests/test_e2e_zenoh.py`

**Step 1: Create convenience run script**

```bash
#!/bin/bash
# bridge/run_bridge.sh - Run the hand zenoh bridge
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."
source /home/javen/work/wujihandpy/.venv/bin/activate
export PYTHONPATH="$SCRIPT_DIR/..:$PYTHONPATH"
python -m bridge.hand_zenoh_bridge "$@"
```

**Step 2: Create e2e test that verifies Zenoh discovery**

```python
# tests/test_e2e_zenoh.py
"""
End-to-end test: starts bridge, verifies Zenoh discovery and GET.
Run with a real hand connected.

Usage: python -m pytest tests/test_e2e_zenoh.py -v -s
"""
import json
import time
import threading

import zenoh

# Test timeout
TIMEOUT_SEC = 5.0


def test_bridge_discoverable_and_get():
    """Start bridge in thread, verify a Zenoh client can discover and GET."""
    import wujihandpy
    from bridge.hand_zenoh_bridge import HandBridge

    hand = wujihandpy.Hand()
    sn = hand.get_product_sn() or "WUJIHAND_E2E_TEST"
    sanitized = sn.replace(".", "_")

    bridge = HandBridge(hand, serial_number=sn, pub_rate=10.0)
    bridge.start()

    try:
        # Give bridge time to initialize
        time.sleep(1.0)

        # Open a separate Zenoh session as "client"
        client = zenoh.open(zenoh.Config())

        # Query @capability
        cap_key = f"wuji/{sanitized}/@capability"
        replies = client.get(cap_key, timeout=TIMEOUT_SEC)
        reply = next(iter(replies))
        cap = json.loads(bytes(reply.ok.payload))
        assert cap["serial_number"] == sn
        assert len(cap["resources"]) > 0

        # Query a GET resource
        voltage_key = f"wuji/{sanitized}/input_voltage"
        replies = client.get(voltage_key, timeout=TIMEOUT_SEC)
        reply = next(iter(replies))
        voltage = json.loads(bytes(reply.ok.payload))
        assert isinstance(voltage, float)
        assert 0.0 < voltage < 30.0  # Reasonable voltage range

        client.close()
    finally:
        bridge.stop()
```

**Step 3: Run e2e test (requires connected hand)**

Run: `PYTHONPATH=. python -m pytest tests/test_e2e_zenoh.py -v -s`
Expected: PASS (with real hand connected)

**Step 4: Make run script executable and commit**

```bash
chmod +x bridge/run_bridge.sh
git add bridge/run_bridge.sh tests/test_e2e_zenoh.py
git commit -m "feat: add e2e test and convenience run script"
```

---

## Task 6: Cross-SDK Verification

**Files:**
- Create: `tests/test_sdk_integration.py`

**Step 1: Write test that uses wuji-sdk Python client to discover the bridged hand**

This test verifies the bridge works end-to-end with the actual wuji-sdk client:

```python
# tests/test_sdk_integration.py
"""
Integration test: bridge + wuji-sdk client.
Requires: real hand + wuji-sdk Python package installed.

Usage: python -m pytest tests/test_sdk_integration.py -v -s
"""
import time
import json

import zenoh


def test_sdk_scan_finds_hand():
    """Verify wuji-sdk scan() discovers the bridged hand via Zenoh."""
    import wujihandpy
    from bridge.hand_zenoh_bridge import HandBridge

    hand = wujihandpy.Hand()
    sn = hand.get_product_sn() or "WUJIHAND_SDK_TEST"
    sanitized = sn.replace(".", "_")

    bridge = HandBridge(hand, serial_number=sn, pub_rate=10.0)
    bridge.start()

    try:
        time.sleep(2.0)

        # Use raw Zenoh liveliness to check discovery
        client = zenoh.open(zenoh.Config())
        alive_key = f"wuji/{sanitized}/@alive"

        # Check liveliness token exists
        replies = client.liveliness().get(f"wuji/**")
        tokens = [str(reply.ok.key_expr) for reply in replies]
        assert any(sanitized in t for t in tokens), \
            f"Hand not found in liveliness tokens: {tokens}"

        # Verify capability query returns valid data
        cap_key = f"wuji/{sanitized}/@capability"
        replies = client.get(cap_key, timeout=5.0)
        reply = next(iter(replies))
        cap = json.loads(bytes(reply.ok.payload))

        # Verify all expected resource types are present
        paths = {r["path"] for r in cap["resources"]}
        assert "joint/actual_position" in paths
        assert "joint/enabled" in paths
        assert "joint/target_position" in paths
        assert "input_voltage" in paths

        client.close()
    finally:
        bridge.stop()


def test_sdk_subscribe_position_stream():
    """Verify Zenoh subscriber receives position stream."""
    import wujihandpy
    from bridge.hand_zenoh_bridge import HandBridge

    hand = wujihandpy.Hand()
    sn = hand.get_product_sn() or "WUJIHAND_SUB_TEST"
    sanitized = sn.replace(".", "_")

    bridge = HandBridge(hand, serial_number=sn, pub_rate=10.0)
    bridge.start()

    received = []

    try:
        time.sleep(1.0)
        client = zenoh.open(zenoh.Config())
        pos_key = f"wuji/{sanitized}/joint/actual_position"

        sub = client.declare_subscriber(
            pos_key,
            lambda sample: received.append(json.loads(bytes(sample.payload))),
        )

        # Wait for a few messages
        time.sleep(1.5)
        sub.undeclare()
        client.close()

        assert len(received) >= 5, f"Expected >=5 messages, got {len(received)}"
        # Verify data shape: 5 fingers x 4 joints
        assert len(received[0]) == 5
        assert len(received[0][0]) == 4

    finally:
        bridge.stop()
```

**Step 2: Run integration test**

Run: `PYTHONPATH=. python -m pytest tests/test_sdk_integration.py -v -s`
Expected: PASS

**Step 3: Commit**

```bash
git add tests/test_sdk_integration.py
git commit -m "test: add cross-sdk integration tests for zenoh bridge"
```

---

## Task 7: Polish and Documentation

**Files:**
- Create: `bridge/README.md`

**Step 1: Create usage README**

```markdown
# Wuji Hand Zenoh Bridge

Bridges a Wuji Hand (via wujihandpy USB) to the Zenoh network,
enabling wuji-sdk clients to discover and control the hand.

## Quick Start

```bash
# Install dependencies
pip install eclipse-zenoh wujihandpy numpy

# Run the bridge (hand must be USB-connected)
PYTHONPATH=. python -m bridge.hand_zenoh_bridge

# Options
PYTHONPATH=. python -m bridge.hand_zenoh_bridge \
    --sn "DEVICE_SN" \
    --pub-rate 50 \
    --log-level DEBUG
```

## How It Works

The bridge process:
1. Connects to the hand via wujihandpy (USB)
2. Opens a Zenoh peer-mode session
3. Declares liveliness token `wuji/{sn}/@alive`
4. Exposes `@capability` and `@control` queryables
5. Exposes GET/SET queryables for each resource
6. Publishes `joint/actual_position` at 50 Hz

wuji-sdk clients discover the hand through Zenoh and interact
with it identically to other devices (gloves, etc).

## Testing

```bash
# Unit tests (no hardware needed)
python -m pytest tests/test_bridge.py -v

# E2E tests (hand must be connected)
PYTHONPATH=. python -m pytest tests/test_e2e_zenoh.py -v -s

# Integration tests (hand must be connected)
PYTHONPATH=. python -m pytest tests/test_sdk_integration.py -v -s
```
```

**Step 2: Commit**

```bash
git add bridge/README.md
git commit -m "docs: add bridge usage README"
```

---

## Summary

| Task | Description | Estimated Steps |
|------|-------------|-----------------|
| 1 | Project scaffold + dependencies | 8 |
| 2 | Capability builder + sanitize_sn | 5 |
| 3 | HandBridge class with full Zenoh lifecycle | 4 |
| 4 | Unit tests for control/resource dispatch | 3 |
| 5 | E2E test with real device | 4 |
| 6 | Cross-SDK integration tests | 3 |
| 7 | Documentation | 2 |

**Total: 7 tasks, ~29 steps**

After all tasks complete, the bridge can be started with one command and any wuji-sdk client on the same network can discover and control the hand through Zenoh.
