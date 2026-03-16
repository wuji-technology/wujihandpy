"""Wuji Hand Zenoh Bridge - exposes wujihandpy via Zenoh for wuji-sdk."""

import json
import time
import threading
import logging
import argparse

import zenoh
import numpy as np

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
        self._control_owner = None
        self._threads = []
        self._queryables = []
        self._publishers = {}
        self._hand_lock = threading.Lock()
        # Allow multi-thread access (we protect with our own lock)
        hand.disable_thread_safe_check()

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

        self._queryables.clear()
        self._publishers.clear()
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
                with self._hand_lock:
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
                with self._hand_lock:
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
                with self._hand_lock:
                    for path, pub in self._publishers.items():
                        value = self._read_resource(path)
                        data = json.dumps(value).encode("utf-8")
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
        hand.write_joint_enabled(False)
        logger.info("Hand disabled, exiting.")


if __name__ == "__main__":
    main()
