"""Tactile Bridge — publishes TouchBoard tactile data to Zenoh for wuji-sdk.

Usage:
    from wujihandpy.bridge import TactileBridge
    bridge = TactileBridge(pub_rate=30)
    bridge.run()  # blocks, Ctrl+C to stop

CLI:
    wujihandpy-tactile-bridge --pub-rate 30
"""

from __future__ import annotations

import json
import time
import logging
from typing import TYPE_CHECKING, Optional

if TYPE_CHECKING:
    from wujihandpy import TouchBoard

import zenoh
import numpy as np

logger = logging.getLogger("tactile_bridge")


class TactileBridge:
    """Bridge TouchBoard tactile data to Zenoh network.

    Publishes:
    - wuji/tboard_{sn}/@alive (liveliness token)
    - wuji/tboard_{sn}/@capability (GET queryable)
    - wuji/tboard_{sn}/tactile (SUB, float32 24x32 at pub_rate Hz)
    - wuji/tboard_{sn}/tactile_raw (SUB, int16 24x32 at pub_rate Hz)
    """

    ROWS = 24
    COLS = 32
    ADC_OPEN_CIRCUIT = 2135.0  # Must match C++ TouchBoard::ADC_OPEN_CIRCUIT

    def __init__(self, serial_number=None, usb_pid=0x5700, pub_rate=30):
        if pub_rate <= 0:
            raise ValueError(f"pub_rate must be positive, got {pub_rate}")
        self.serial_number = serial_number
        self.usb_pid = usb_pid
        self.pub_rate = pub_rate
        self._running = False
        self._bridge_id: Optional[str] = None
        self._tb: Optional["TouchBoard"] = None
        self._session = None
        self._alive_token = None
        self._queryable = None

    def _sanitize_sn(self, sn):
        return sn.replace(".", "_") if sn else "unknown"

    def _key(self, suffix):
        bridge_id = self._bridge_id or self._sanitize_sn(self.serial_number or "tboard")
        return f"wuji/tboard_{bridge_id}/{suffix}"

    def start(self):
        """Connect to TouchBoard and start publishing.

        On any failure, calls stop() to clean up partially initialized resources.
        """
        from wujihandpy import TouchBoard

        try:
            logger.info("Connecting to TouchBoard...")
            self._tb = TouchBoard(
                serial_number=self.serial_number,
                usb_pid=self.usb_pid,
            )

            # Wait for first frame (only tolerate timeout, not other errors)
            try:
                self._tb.read_tactile(timeout=5.0)
            except TimeoutError:
                logger.warning("No initial frame within 5s, continuing anyway")

            handedness = self._tb.handedness
            hand_str = {0: "left", 1: "right"}.get(handedness, "unknown")
            logger.info(
                f"TouchBoard connected: handedness={handedness}, fps={self._tb.fps:.0f}"
            )

            # Compute canonical bridge ID once; used by _key() for Zenoh key generation.
            # _key() adds "wuji/tboard_" prefix, so bridge_id should NOT include "tboard_".
            self._bridge_id = self._sanitize_sn(
                self.serial_number or hand_str
            )

            # Open Zenoh session
            config = zenoh.Config()
            self._session = zenoh.open(config)

            # Declare liveliness token
            self._alive_token = self._session.liveliness().declare_token(
                self._key("@alive")
            )
            logger.info(f"Liveliness: {self._key('@alive')}")

            # Declare capability queryable — follow wuji-sdk protocol (same as HandBridge)
            # capability.serial_number preserves the original serial for human readability,
            # while _bridge_id (used by _key()) is sanitized for safe Zenoh key naming.
            sn = self.serial_number or f"tboard_{hand_str}"
            resources = [
                {
                    "path": "tactile",
                    "schema_id": 0,
                    "can_get": False,
                    "can_set": False,
                    "can_sub": True,
                    "can_pub": False,
                    "can_exec": False,
                    "internal": False,
                    "serde_format": "json",
                    "json_schema": {
                        "title": "TactileTimestamped",
                        "type": "object",
                        "properties": {
                            "timestamp_us": {"type": "integer"},
                            "data": {
                                "type": "array",
                                "items": {"type": "number"},
                                "description": f"Normalized pressure {self.ROWS}x{self.COLS} (0.0-1.0)",
                            },
                        },
                        "required": ["timestamp_us", "data"],
                    },
                },
                {
                    "path": "tactile_raw",
                    "schema_id": 0,
                    "can_get": False,
                    "can_set": False,
                    "can_sub": True,
                    "can_pub": False,
                    "can_exec": False,
                    "internal": False,
                    "serde_format": "json",
                    "json_schema": {
                        "title": "TactileRawTimestamped",
                        "type": "object",
                        "properties": {
                            "timestamp_us": {"type": "integer"},
                            "data": {
                                "type": "array",
                                "items": {"type": "integer"},
                                "description": f"Raw ADC values {self.ROWS}x{self.COLS}",
                            },
                        },
                        "required": ["timestamp_us", "data"],
                    },
                },
                {
                    "path": "handedness",
                    "schema_id": 0,
                    "can_get": False,
                    "can_set": False,
                    "can_sub": False,
                    "can_pub": False,
                    "can_exec": False,
                    "internal": False,
                    "serde_format": "json",
                    "json_schema": {"title": "Handedness", "type": "string"},
                },
            ]

            capability = {
                "device_id": 0,
                "device_proto": "custom",
                "firmware_version": "",
                "serial_number": sn,
                "nodes": [],
                "resources": resources,
                # Extra metadata for consumers
                "device_type": "touch_board",
                "rows": self.ROWS,
                "cols": self.COLS,
                "handedness": handedness,
            }

            cap_json = json.dumps(capability)
            self._queryable = self._session.declare_queryable(
                self._key("@capability"),
                lambda query: query.reply(self._key("@capability"), cap_json.encode()),
            )
            logger.info(f"Capability queryable: {self._key('@capability')}")

            self._running = True
            logger.info(f"Publishing at {self.pub_rate} Hz")
        except Exception:
            self.stop()
            raise

    def run(self):
        """Start and run the bridge (blocking). Ctrl+C to stop."""
        self.start()  # start() handles its own cleanup on failure

        period = 1.0 / self.pub_rate
        next_time = time.monotonic() + period
        pub_count = 0
        last_stats = time.monotonic()

        try:
            while self._running:
                raw = self._tb.get_tactile_raw()

                if raw is not None:
                    data = np.clip(1.0 - raw / self.ADC_OPEN_CIRCUIT, 0.0, 1.0).astype(np.float32)
                    timestamp_us = int(time.time() * 1_000_000)
                    payload = json.dumps(
                        {
                            "timestamp_us": timestamp_us,
                            "data": data.flatten().tolist(),
                        }
                    )
                    self._session.put(self._key("tactile"), payload.encode())

                    payload_raw = json.dumps(
                        {
                            "timestamp_us": timestamp_us,
                            "data": raw.flatten().tolist(),
                        }
                    )
                    self._session.put(self._key("tactile_raw"), payload_raw.encode())
                    pub_count += 1

                # Stats every 5 seconds
                now = time.monotonic()
                if now - last_stats >= 5.0:
                    elapsed = now - last_stats
                    logger.info(
                        f"Pub rate: {pub_count / elapsed:.1f} Hz, "
                        f"TouchBoard FPS: {self._tb.fps:.0f}"
                    )
                    pub_count = 0
                    last_stats = now

                # Deadline-based sleep (avoid drift)
                sleep_time = next_time - time.monotonic()
                if sleep_time > 0:
                    time.sleep(sleep_time)
                    next_time += period
                else:
                    # Overrun: skip missed intervals instead of burst catch-up
                    next_time = time.monotonic() + period

        except KeyboardInterrupt:
            logger.info("Interrupted")
        finally:
            self.stop()

    def stop(self):
        """Stop the bridge and clean up."""
        self._running = False
        if self._queryable is not None:
            try:
                self._queryable.undeclare()
            except Exception as e:
                logger.debug("Error undeclaring queryable: %s", e)
            self._queryable = None
        if self._alive_token is not None:
            try:
                self._alive_token.undeclare()
            except Exception as e:
                logger.debug("Error undeclaring alive token: %s", e)
            self._alive_token = None
        if self._session is not None:
            try:
                self._session.close()
            except Exception as e:
                logger.debug("Error closing Zenoh session: %s", e)
            self._session = None
        self._tb = None
        logger.info("TactileBridge stopped")


def main():
    """CLI entry point for tactile bridge."""
    import argparse

    parser = argparse.ArgumentParser(description="TouchBoard Tactile Bridge")
    parser.add_argument("--sn", default=None, help="TouchBoard serial number")
    parser.add_argument(
        "--pub-rate", type=int, default=30, help="Publish rate Hz (default: 30)"
    )
    parser.add_argument(
        "--usb-pid", type=lambda x: int(x, 0), default=0x5700, help="USB PID"
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(name)s] %(levelname)s: %(message)s",
    )

    bridge = TactileBridge(
        serial_number=args.sn, usb_pid=args.usb_pid, pub_rate=args.pub_rate
    )
    bridge.run()


if __name__ == "__main__":
    main()
