"""Wuji Hand Zenoh Bridge - exposes wujihandpy via Zenoh for wuji-sdk.

Uses realtime_controller internally for smooth motion control (PDO 1kHz)
instead of SDO request/response for target_position writes.
"""

import json
import time
import threading
import logging
import argparse

import zenoh
import numpy as np

# ---------------------------------------------------------------------------
# Timestamp utility
# ---------------------------------------------------------------------------

def get_timestamp_us() -> int:
    """Return current UTC time as microseconds since Unix epoch."""
    return time.time_ns() // 1000


def wrap_with_timestamp(value, timestamp_us: int = None) -> dict:
    """Wrap a data value with a host-side timestamp.

    Output format:
        {"timestamp_us": <int>, "data": <value>}

    This aligns wujihandpy bridge output with wuji-sdk's timestamped data model,
    using host-side UTC timestamps (since the CANopen PDO protocol has no device
    timestamps).
    """
    if timestamp_us is None:
        timestamp_us = get_timestamp_us()
    return {"timestamp_us": timestamp_us, "data": value}

logger = logging.getLogger("hand_bridge")


def sanitize_sn(sn: str) -> str:
    """Replace dots with underscores for Zenoh key expressions."""
    return sn.replace(".", "_")


# Resource definitions
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
    {
        "path": "joint/bus_voltage",
        "can_get": True, "can_set": False, "can_sub": False,
        "json_schema": {
            "title": "JointBusVoltage",
            "type": "array",
            "description": "5x4 joint bus voltages",
            "items": {"type": "array", "items": {"type": "number"}},
        },
    },
    {
        "path": "joint/actual_effort",
        "can_get": True, "can_set": False, "can_sub": True,
        "json_schema": {
            "title": "JointActualEffort",
            "type": "array",
            "description": "5x4 joint actual effort (requires firmware >= 1.2.0)",
            "items": {"type": "array", "items": {"type": "number"}},
        },
    },
    # SET resources (require control)
    {
        "path": "joint/reset_error",
        "can_get": False, "can_set": True, "can_sub": False,
        "json_schema": {
            "title": "JointResetError",
            "type": "array",
            "description": "5x4 joint error reset (write non-zero to reset)",
            "items": {"type": "array", "items": {"type": "integer"}},
        },
    },
    {
        "path": "joint/control_mode",
        "can_get": False, "can_set": True, "can_sub": False,
        "json_schema": {
            "title": "JointControlMode",
            "type": "array",
            "description": "5x4 joint control modes",
            "items": {"type": "array", "items": {"type": "integer"}},
        },
    },
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

    # Wrap SUB resource schemas with timestamp envelope
    for res in resources:
        if res["can_sub"] and res.get("json_schema"):
            original_schema = res["json_schema"]
            res["json_schema"] = {
                "title": original_schema.get("title", "") + "Timestamped",
                "type": "object",
                "description": "Host-timestamped envelope: {timestamp_us, data}",
                "properties": {
                    "timestamp_us": {"type": "integer", "description": "UTC microseconds since epoch"},
                    "data": original_schema,
                },
                "required": ["timestamp_us", "data"],
            }

    capability = {
        "device_id": 0,
        "device_proto": "custom",
        "firmware_version": "",
        "serial_number": serial_number,
        "nodes": [],
        "resources": resources,
    }
    return json.dumps(capability)


class HandBridge:
    """Bridge between wujihandpy and Zenoh network.

    Uses realtime_controller for target_position writes (PDO 1kHz with
    LowPass interpolation) instead of SDO for smooth motion control.
    """

    def __init__(self, hand, serial_number: str, pub_rate: float = 50.0):
        self.hand = hand
        self.sn = serial_number
        self.sanitized_sn = sanitize_sn(serial_number)
        self.pub_rate = pub_rate
        self.session = None
        self._alive_token = None
        self._running = False
        self._control_owner = None
        self._control_owner_watcher = None  # liveliness subscriber for owner TTL
        self._threads = []
        self._queryables = []
        self._publishers = {}
        self._hand_lock = threading.Lock()
        self._control_lock = threading.Lock()
        # Realtime controller state
        self._controller = None
        self._rt_target = np.zeros((5, 4), dtype=np.float64)
        self._rt_lock = threading.Lock()
        # Allow multi-thread access (we protect with our own lock)
        hand.disable_thread_safe_check()

    def _key(self, suffix: str) -> str:
        return f"wuji/{self.sanitized_sn}/{suffix}"

    def _control_owner_key(self, owner_zid: str) -> str:
        """Liveliness key for tracking a control owner's presence."""
        return f"wuji/{self.sanitized_sn}/@control_owner/{owner_zid}"

    def _start_owner_watcher(self, owner_zid: str):
        """Start watching owner's liveliness. Auto-release control if owner crashes."""
        self._stop_owner_watcher()

        owner_key = self._control_owner_key(owner_zid)
        try:
            def on_sample(sample):
                # SampleKind.DELETE means the liveliness token was dropped (owner crashed)
                if hasattr(sample, 'kind') and str(sample.kind).endswith('Delete'):
                    with self._control_lock:
                        logger.warning(f"Control owner {owner_zid} crashed, auto-releasing")
                        self._control_owner = None
                    self._stop_owner_watcher()

            self._control_owner_watcher = self.session.liveliness().declare_subscriber(
                owner_key, on_sample
            )
            logger.debug(f"Owner watcher started for {owner_zid}")
        except Exception as e:
            logger.warning(f"Failed to start owner watcher: {e}")
            self._control_owner_watcher = None

    def _stop_owner_watcher(self):
        """Stop watching the current owner's liveliness."""
        if self._control_owner_watcher is not None:
            try:
                self._control_owner_watcher.undeclare()
            except Exception:
                pass
            self._control_owner_watcher = None

    def _start_realtime_controller(self):
        """Enable joints and start realtime controller (raw passthrough)."""
        import wujihandpy

        # Set control mode to RT_FCL (9) for force-closed-loop, matching HMI behavior
        RT_FCL_MODE = 9
        logger.info("Setting control mode to RT_FCL (%d)...", RT_FCL_MODE)
        self.hand.write_joint_control_mode(np.full((5, 4), RT_FCL_MODE, dtype=np.int32))
        time.sleep(0.5)

        logger.info("Enabling all joints...")
        self.hand.write_joint_enabled(True)
        time.sleep(0.3)

        # Read initial position as starting target
        initial_pos = self.hand.read_joint_actual_position()
        with self._rt_lock:
            self._rt_target = initial_pos.copy()

        # Use an extremely high cutoff so the filter is effectively a passthrough.
        # realtime_controller() requires an IFilter; there is no identity filter.
        logger.info("Starting realtime controller (no filtering, upstream enabled)...")
        self._controller = self.hand.realtime_controller(
            enable_upstream=True,
            filter=wujihandpy.filter.LowPass(cutoff_freq=10000.0),
        )
        self._controller.__enter__()

        # Start the realtime feed loop (100Hz -> 1kHz via controller interpolation)
        t = threading.Thread(target=self._realtime_loop, daemon=True)
        t.start()
        self._threads.append(t)
        logger.info("Realtime controller started")

    def _stop_realtime_controller(self):
        """Stop realtime controller and disable joints."""
        if self._controller is not None:
            logger.info("Stopping realtime controller...")
            # Set target to zero before stopping
            with self._rt_lock:
                self._rt_target = np.zeros((5, 4), dtype=np.float64)
            self._controller.set_joint_target_position(self._rt_target)
            time.sleep(1.0)

            self._controller.__exit__(None, None, None)
            self._controller = None
            logger.info("Realtime controller stopped")

        logger.info("Disabling all joints...")
        self.hand.write_joint_enabled(False)

    def _realtime_loop(self):
        """Feed target position to realtime controller at 100Hz."""
        period = 1.0 / 100.0
        while self._running and self._controller is not None:
            try:
                with self._rt_lock:
                    target = self._rt_target.copy()
                self._controller.set_joint_target_position(target)
            except Exception as e:
                logger.error(f"Realtime loop error: {e}")
            time.sleep(period)

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
        self._queryables.append(self.session.declare_queryable(
            self._key("@capability"),
            lambda query, data=cap_bytes: query.reply(self._key("@capability"), data),
        ))
        logger.info("@capability queryable declared")

        # 4. Control queryable
        self._queryables.append(self.session.declare_queryable(
            self._key("@control"),
            self._handle_control,
        ))
        logger.info("@control queryable declared")

        # 5. Resource queryables (GET/SET)
        for r in RESOURCE_DEFS:
            if r["can_get"] or r["can_set"]:
                q = self.session.declare_queryable(
                    self._key(r["path"]),
                    lambda query, res=r: self._handle_resource_query(query, res),
                )
                self._queryables.append(q)
                logger.info(f"Resource queryable: {r['path']}")

        # 6. SUB publishers (continuous streams)
        self._running = True
        for r in RESOURCE_DEFS:
            if r["can_sub"]:
                pub = self.session.declare_publisher(self._key(r["path"]))
                self._publishers[r["path"]] = pub

        # 7. Start realtime controller for smooth motion
        self._start_realtime_controller()

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

        # Stop realtime controller
        self._stop_realtime_controller()

        # Stop owner watcher
        self._stop_owner_watcher()

        if self.session:
            self.session.put(self._key("@status"), b"offline")
            logger.info("Status: offline")

        self._queryables.clear()
        self._publishers.clear()
        self._alive_token = None
        self.session = None
        logger.info("Bridge stopped")

    def _handle_control(self, query):
        """Handle @control acquire/release protocol with liveliness-based TTL.

        When control is acquired, a liveliness subscriber watches the owner's
        presence. If the owner process crashes (liveliness token dropped),
        control is automatically released.
        """
        key = self._key("@control")
        payload = bytes(query.payload) if query.payload else b""
        payload_str = payload.decode("utf-8", errors="replace")

        if payload_str.startswith("acquire:"):
            requester = payload_str[len("acquire:"):]
            with self._control_lock:
                if self._control_owner is None or self._control_owner == requester:
                    self._control_owner = requester
                    query.reply(key, b"granted")
                    logger.info(f"Control granted to {requester}")
                    # Start liveliness watcher for auto-release on crash
                    self._start_owner_watcher(requester)
                else:
                    query.reply(key, f"denied:{self._control_owner}".encode())
                    logger.info(f"Control denied to {requester}, owner: {self._control_owner}")
        elif payload_str.startswith("release:"):
            requester = payload_str[len("release:"):]
            with self._control_lock:
                if self._control_owner == requester:
                    self._control_owner = None
                    self._stop_owner_watcher()
                    query.reply(key, b"released")
                    logger.info(f"Control released by {requester}")
                else:
                    query.reply(key, b"not_owner")
        else:
            with self._control_lock:
                owner = self._control_owner or "none"
            query.reply(key, owner.encode())

    def _handle_resource_query(self, query, resource_def):
        """Handle GET/SET for a resource."""
        key = self._key(resource_def["path"])
        payload = bytes(query.payload) if query.payload else b""

        if len(payload) == 0:
            # GET — response includes host-side timestamp
            if not resource_def["can_get"]:
                query.reply_err(b"GET not supported")
                return
            try:
                value = self._read_resource(resource_def["path"])
                envelope = wrap_with_timestamp(value)
                data = json.dumps(envelope).encode("utf-8")
                query.reply(key, data)
            except Exception as e:
                logger.error(f"GET {resource_def['path']} failed: {e}")
                query.reply_err(str(e).encode())
        else:
            # SET
            if not resource_def["can_set"]:
                query.reply_err(b"SET not supported")
                return
            with self._control_lock:
                has_owner = self._control_owner is not None
            if not has_owner:
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
        """Read a resource from the hand, return JSON-serializable value.

        For actual_position, reads from realtime controller cache (non-blocking)
        when available. Other resources use SDO with hand_lock.
        """
        if path == "joint/actual_position" and self._controller is not None:
            # Zero-copy from controller cache, no SDO needed
            return self._controller.get_joint_actual_position().tolist()

        if path == "joint/actual_effort" and self._controller is not None:
            return self._controller.get_joint_actual_effort().tolist()

        # All other reads need SDO access via hand_lock
        with self._hand_lock:
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
            elif path == "joint/bus_voltage":
                return self.hand.read_joint_bus_voltage().tolist()
            else:
                raise ValueError(f"Unknown GET resource: {path}")

    def _write_resource(self, path: str, value):
        """Write a resource to the hand.

        For target_position, updates the realtime controller target atomically
        (non-blocking). Other writes use SDO with hand_lock.
        """
        if path == "joint/target_position":
            # Atomically update realtime target - no SDO, no blocking
            with self._rt_lock:
                self._rt_target = np.array(value, dtype=np.float64)
            return

        # Other writes need SDO access
        with self._hand_lock:
            if path == "joint/control_mode":
                self.hand.write_joint_control_mode(np.array(value, dtype=np.int32))
            elif path == "joint/enabled":
                self.hand.write_joint_enabled(np.array(value, dtype=bool))
            elif path == "joint/effort_limit":
                self.hand.write_joint_effort_limit(np.array(value, dtype=np.float64))
            elif path == "joint/reset_error":
                self.hand.write_joint_reset_error(np.array(value, dtype=np.uint16))
            else:
                raise ValueError(f"Unknown SET resource: {path}")

    def _publish_loop(self):
        """Continuously publish SUB resources at configured rate.

        Each published message is wrapped with a host-side timestamp:
            {"timestamp_us": <UTC microseconds>, "data": <value>}
        """
        period = 1.0 / self.pub_rate
        while self._running:
            try:
                timestamp_us = get_timestamp_us()
                for path, pub in self._publishers.items():
                    value = self._read_resource(path)
                    envelope = wrap_with_timestamp(value, timestamp_us)
                    data = json.dumps(envelope).encode("utf-8")
                    pub.put(data)
            except Exception as e:
                logger.error(f"Publish loop error: {e}")
            time.sleep(period)


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

    try:
        sn = hand.get_product_sn() or f"WUJIHAND_{id(hand):08X}"
    except Exception:
        sn = f"WUJIHAND_{id(hand):08X}"
        logger.warning(f"Could not read product SN (firmware too old?), using: {sn}")
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
        logger.info("Exiting.")


if __name__ == "__main__":
    main()
