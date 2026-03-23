"""Wuji Hand Zenoh Bridge - exposes wujihandpy via Zenoh for wuji-sdk.

Uses realtime_controller internally for smooth motion control (PDO 1kHz)
instead of SDO request/response for target_position writes.
"""

import json
import time
import threading
import logging
import argparse
from typing import Optional

import zenoh
import numpy as np

# ---------------------------------------------------------------------------
# Timestamp utility
# ---------------------------------------------------------------------------

def get_timestamp_us() -> int:
    """Return current UTC time as microseconds since Unix epoch."""
    return time.time_ns() // 1000


def wrap_with_timestamp(value, timestamp_us: Optional[int] = None) -> dict:
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


def decode_zenoh_text(value) -> Optional[str]:
    """Best-effort decode for Zenoh payload/attachment text fields."""
    if value is None:
        return None
    if isinstance(value, str):
        return value
    if isinstance(value, (bytes, bytearray)):
        return bytes(value).decode("utf-8", errors="replace")

    to_string = getattr(value, "to_string", None)
    if callable(to_string):
        decoded = to_string()
        if isinstance(decoded, str):
            return decoded

    to_bytes = getattr(value, "to_bytes", None)
    if callable(to_bytes):
        decoded = to_bytes()
        if isinstance(decoded, (bytes, bytearray)):
            return bytes(decoded).decode("utf-8", errors="replace")

    try:
        return bytes(value).decode("utf-8", errors="replace")
    except (TypeError, ValueError):
        return None

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

    def __init__(self, hand, serial_number: str, pub_rate: float):
        """Initialize the Hand Zenoh Bridge.

        Args:
            hand: A wujihandpy.Hand instance (USB-connected).
            serial_number: Device serial number for Zenoh key registration.
            pub_rate: SUB resource publish rate in Hz (e.g. 1000). Must be positive.

        Raises:
            ValueError: If pub_rate is not positive.
        """
        if pub_rate <= 0:
            raise ValueError(f"pub_rate must be positive, got {pub_rate}")
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
        self._subscribers = []
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
        """Build a Zenoh key expression: wuji/{sanitized_sn}/{suffix}."""
        return f"wuji/{self.sanitized_sn}/{suffix}"

    def _control_owner_key(self, owner_zid: str) -> str:
        """Liveliness key for tracking a control owner's presence."""
        return f"wuji/{self.sanitized_sn}/@control_owner/{owner_zid}"

    def _start_owner_watcher(self, owner_zid: str):
        """Start watching owner's liveliness. Auto-release control if owner crashes."""
        self._stop_owner_watcher()

        owner_key = self._control_owner_key(owner_zid)

        def on_sample(sample):
            """Handle liveliness change; auto-release control on owner crash."""
            # SampleKind.DELETE means the liveliness token was dropped (owner crashed)
            if hasattr(sample, "kind") and sample.kind == zenoh.SampleKind.DELETE:
                with self._control_lock:
                    if self._control_owner == owner_zid:
                        logger.warning(f"Control owner {owner_zid} crashed, auto-releasing")
                        self._control_owner = None
                self._stop_owner_watcher()

        try:
            watcher = self.session.liveliness().declare_subscriber(owner_key, on_sample)
            if watcher is None:
                raise RuntimeError(f"declare_subscriber returned None for {owner_key}")
            self._control_owner_watcher = watcher
            logger.debug(f"Owner watcher started for {owner_zid}")
        except Exception:
            self._control_owner_watcher = None
            raise

    def _stop_owner_watcher(self):
        """Stop watching the current owner's liveliness."""
        if self._control_owner_watcher is not None:
            try:
                self._control_owner_watcher.undeclare()
            except Exception as e:
                logger.debug(f"Failed to undeclare owner watcher: {e}")
            self._control_owner_watcher = None

    def _undeclare(self, entity, label: str):
        """Best-effort undeclare for Zenoh entities during shutdown."""
        if entity is None:
            return
        try:
            entity.undeclare()
        except Exception as e:
            logger.debug(f"Failed to undeclare {label}: {e}")

    def _close_session(self, session):
        """Best-effort session close used by stop/startup rollback."""
        if session is None:
            return
        close = getattr(session, "close", None)
        if close is None:
            return
        try:
            close()
        except Exception as e:
            logger.debug(f"Failed to close Zenoh session: {e}")

    def _get_requester_id(self, message) -> Optional[str]:
        """Extract requester ZID from a query/sample attachment."""
        requester = decode_zenoh_text(getattr(message, "attachment", None))
        if requester:
            return requester
        return None

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
        controller = self._controller
        if controller is not None:
            logger.info("Stopping realtime controller...")
            # Set target to zero before stopping
            with self._rt_lock:
                self._rt_target = np.zeros((5, 4), dtype=np.float64)
                target = self._rt_target.copy()
            controller.set_joint_target_position(target)
            time.sleep(1.0)

            controller.__exit__(None, None, None)
            self._controller = None
            logger.info("Realtime controller stopped")

        logger.info("Disabling all joints...")
        self.hand.write_joint_enabled(False)

    def _realtime_loop(self):
        """Feed target position to realtime controller at 100Hz."""
        period = 1.0 / 100.0
        while self._running:
            try:
                with self._rt_lock:
                    target = self._rt_target.copy()
                    controller = self._controller
                if controller is None:
                    break
                controller.set_joint_target_position(target)
            except Exception as e:
                logger.error(f"Realtime loop error: {e}")
            time.sleep(period)

    def start(self):
        """Open Zenoh session, declare liveliness, publish status, start queryables."""
        try:
            logger.info("Opening Zenoh session...")
            config = zenoh.Config()
            self.session = zenoh.open(config)
            zid = str(self.session.zid())
            logger.info(f"Zenoh session opened, ZID: {zid}")

            # 1. Liveliness token
            self._alive_token = self.session.liveliness().declare_token(self._key("@alive"))
            logger.info(f"Liveliness token declared: {self._key('@alive')}")

            # 2. Start realtime controller BEFORE exposing queryables
            self._running = True
            self._start_realtime_controller()

            # 3. Status: online (after controller is ready)
            self.session.put(self._key("@status"), b"online")
            logger.info("Status: online")

            # 4. Capability queryable
            cap_bytes = build_capability(self.sn).encode("utf-8")
            self._queryables.append(self.session.declare_queryable(
                self._key("@capability"),
                lambda query, data=cap_bytes: query.reply(self._key("@capability"), data),
            ))
            logger.info("@capability queryable declared")

            # 5. Control queryable
            self._queryables.append(self.session.declare_queryable(
                self._key("@control"),
                self._handle_control,
            ))
            logger.info("@control queryable declared")

            # 6. Resource queryables (GET/SET)
            for r in RESOURCE_DEFS:
                if r["can_get"] or r["can_set"]:
                    q = self.session.declare_queryable(
                        self._key(r["path"]),
                        lambda query, res=r: self._handle_resource_query(query, res),
                    )
                    self._queryables.append(q)
                    logger.info(f"Resource queryable: {r['path']}")

            # 7. Subscribe to target_position for fire-and-forget writes (low latency)
            target_pos_sub = self.session.declare_subscriber(
                self._key("joint/target_position"),
                self._handle_target_position_put,
            )
            self._subscribers.append(target_pos_sub)
            logger.info("target_position subscriber declared (fire-and-forget path)")

            # 8. SUB publishers (continuous streams)
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
        except Exception:
            logger.exception("Bridge startup failed, cleaning up partial state")
            self.stop()
            raise

    def stop(self):
        """Gracefully shutdown."""
        logger.info("Stopping bridge...")
        self._running = False

        for t in self._threads:
            try:
                t.join(timeout=2.0)
            except Exception as e:
                logger.debug(f"Failed to join bridge thread: {e}")
        self._threads.clear()

        try:
            self._stop_realtime_controller()
        except Exception as e:
            logger.warning(f"Failed to stop realtime controller: {e}")

        with self._control_lock:
            self._control_owner = None
        self._stop_owner_watcher()

        session = self.session
        if session is not None:
            try:
                session.put(self._key("@status"), b"offline")
                logger.info("Status: offline")
            except Exception as e:
                logger.debug(f"Failed to publish offline status: {e}")

        for idx, queryable in enumerate(self._queryables):
            self._undeclare(queryable, f"queryable[{idx}]")
        for idx, subscriber in enumerate(self._subscribers):
            self._undeclare(subscriber, f"subscriber[{idx}]")
        for path, publisher in self._publishers.items():
            self._undeclare(publisher, f"publisher[{path}]")
        self._undeclare(self._alive_token, "alive token")

        self._queryables.clear()
        self._subscribers.clear()
        self._publishers.clear()
        self._alive_token = None
        self.session = None
        self._close_session(session)
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
            attachment_requester = self._get_requester_id(query)
            if attachment_requester != requester:
                query.reply_err(b"identity_mismatch")
                logger.warning(
                    "Control acquire rejected: payload requester %r != attachment requester %r",
                    requester,
                    attachment_requester,
                )
                return
            with self._control_lock:
                current_owner = self._control_owner
                if current_owner is not None and current_owner != requester:
                    query.reply(key, f"denied:{current_owner}".encode())
                    logger.info(f"Control denied to {requester}, owner: {current_owner}")
                    return
                # Reserve control so competing acquire requests stay serialized while
                # we establish the liveliness watcher.
                self._control_owner = requester

            try:
                self._start_owner_watcher(requester)
                with self._control_lock:
                    if self._control_owner != requester:
                        raise RuntimeError("control owner lost before acquire completed")
                query.reply(key, b"granted")
                logger.info(f"Control granted to {requester}")
            except Exception as e:
                with self._control_lock:
                    if self._control_owner == requester:
                        self._control_owner = None
                self._stop_owner_watcher()
                query.reply_err(str(e).encode())
                logger.error(f"Control acquire failed for {requester}: {e}")
        elif payload_str.startswith("release:"):
            requester = payload_str[len("release:"):]
            attachment_requester = self._get_requester_id(query)
            if attachment_requester != requester:
                query.reply_err(b"identity_mismatch")
                logger.warning(
                    "Control release rejected: payload requester %r != attachment requester %r",
                    requester,
                    attachment_requester,
                )
                return
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
            # GET replies keep the resource's original JSON schema.
            if not resource_def["can_get"]:
                query.reply_err(b"GET not supported")
                return
            try:
                value = self._read_resource(resource_def["path"])
                query.reply(key, json.dumps(value).encode("utf-8"))
            except Exception as e:
                logger.error(f"GET {resource_def['path']} failed: {e}")
                query.reply_err(str(e).encode())
        else:
            # SET
            if not resource_def["can_set"]:
                query.reply_err(b"SET not supported")
                return
            requester = self._get_requester_id(query)
            with self._control_lock:
                owner = self._control_owner
            if owner is None:
                query.reply_err(b"no control owner")
                return
            if requester is None:
                query.reply_err(b"missing requester id")
                logger.warning(f"SET {resource_def['path']} rejected: missing requester attachment")
                return
            if requester != owner:
                query.reply_err(b"not control owner")
                logger.warning(
                    "SET %s rejected: requester %s != owner %s",
                    resource_def["path"],
                    requester,
                    owner,
                )
                return
            try:
                value = json.loads(payload.decode("utf-8"))
                self._write_resource(resource_def["path"], value)
                query.reply(key, b'"ok"')
            except Exception as e:
                logger.error(f"SET {resource_def['path']} failed: {e}")
                query.reply_err(str(e).encode())

    def _handle_target_position_put(self, sample):
        """Handle fire-and-forget PUT for target_position (low-latency path)."""
        try:
            requester = self._get_requester_id(sample)
            with self._control_lock:
                owner = self._control_owner
            if owner is None:
                logger.warning("Ignoring target_position PUT without control owner")
                return
            if requester is None:
                logger.warning("Ignoring target_position PUT without requester attachment")
                return
            if requester != owner:
                logger.warning(
                    "Ignoring target_position PUT from non-owner requester %s (owner=%s)",
                    requester,
                    owner,
                )
                return
            value = json.loads(bytes(sample.payload).decode("utf-8"))
            self._write_resource("joint/target_position", value)
        except ValueError as e:
            logger.warning(f"Invalid target_position PUT ignored: {e}")
        except Exception as e:
            logger.error(f"target_position subscriber error: {e}")

    def _read_resource(self, path: str):
        """Read a resource from the hand, return JSON-serializable value.

        For actual_position, reads from realtime controller cache (non-blocking)
        when available. Other resources use SDO with hand_lock.
        """
        controller = self._controller

        if path == "joint/actual_position" and controller is not None:
            # Zero-copy from controller cache, no SDO needed
            return controller.get_joint_actual_position().tolist()

        if path == "joint/actual_effort":
            if controller is not None:
                return controller.get_joint_actual_effort().tolist()
            # actual_effort is only exposed from the realtime controller cache;
            # during shutdown races, return a zeroed snapshot instead of failing.
            return np.zeros((5, 4), dtype=np.float64).tolist()

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
            arr = np.asarray(value, dtype=np.float64)
            if arr.shape != (5, 4):
                raise ValueError(f"target_position must be 5x4 array, got shape {arr.shape}")
            if not np.isfinite(arr).all():
                raise ValueError("target_position contains non-finite values")
            # Atomically update realtime target - no SDO, no blocking
            with self._rt_lock:
                self._rt_target = arr
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
    """Entry point: parse args, connect to hand, start Zenoh bridge."""
    parser = argparse.ArgumentParser(description="Wuji Hand Zenoh Bridge")
    parser.add_argument("--sn", type=str, default=None, help="Hand serial number filter")
    parser.add_argument("--pub-rate", type=float, required=True, help="Position publish rate in Hz (e.g. 1000)")
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
    try:
        bridge.start()
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
