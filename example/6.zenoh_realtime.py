"""
Zenoh Bridge Realtime Control Test

Connects to the hand through Zenoh (not direct USB) and runs a sine wave
on fingers F2-F5, similar to wujihandpy's 3.realtime.py example.

Prerequisites:
  1. Hand connected via USB
  2. Bridge running: PYTHONPATH=. python -m bridge.python.hand_zenoh_bridge --pub-rate 1000

Usage:
  python example/zenoh_realtime.py [--sn WUJIHAND_001] [--duration 10] [--rate 50]
"""

import json
import time
import math
import argparse
import threading

import zenoh


def find_hand(session: zenoh.Session, sn: str | None, timeout: float = 5.0) -> str:
    """Discover a hand on the Zenoh network, return its sanitized SN."""
    if sn:
        # Verify it exists
        replies = session.get(f"wuji/{sn}/@capability", timeout=timeout)
        for reply in replies:
            cap = json.loads(bytes(reply.ok.payload))
            print(f"Found hand: {cap['serial_number']} ({len(cap['resources'])} resources)")
            return sn
        raise RuntimeError(f"Hand '{sn}' not found on Zenoh network")

    # Auto-discover via liveliness
    print("Scanning for hands on Zenoh network...")
    replies = session.liveliness().get("wuji/**")
    for reply in replies:
        key = str(reply.ok.key_expr)
        if "/@alive" in key:
            discovered_sn = key.split("/")[1]
            print(f"Discovered: {discovered_sn}")
            return discovered_sn

    raise RuntimeError("No hand found on Zenoh network")


def acquire_control(session: zenoh.Session, sn: str) -> str:
    """Acquire write control, return ZID."""
    zid = str(session.zid())
    replies = session.get(
        f"wuji/{sn}/@control",
        payload=f"acquire:{zid}".encode(),
        timeout=5.0,
    )
    for reply in replies:
        result = bytes(reply.ok.payload).decode()
        if result == "granted":
            print(f"Control acquired (ZID: {zid[:8]}...)")
            return zid
        raise RuntimeError(f"Control denied: {result}")
    raise RuntimeError("No reply from @control")


def release_control(session: zenoh.Session, sn: str, zid: str):
    """Release write control."""
    replies = session.get(
        f"wuji/{sn}/@control",
        payload=f"release:{zid}".encode(),
        timeout=5.0,
    )
    for reply in replies:
        print(f"Control released: {bytes(reply.ok.payload).decode()}")


def set_resource(session: zenoh.Session, sn: str, path: str, value):
    """SET a resource via Zenoh queryable."""
    data = json.dumps(value).encode("utf-8")
    replies = session.get(f"wuji/{sn}/{path}", payload=data, timeout=5.0)
    for reply in replies:
        result = bytes(reply.ok.payload).decode()
        if result != '"ok"':
            raise RuntimeError(f"SET {path} failed: {result}")


def unwrap_envelope(payload):
    """Unwrap timestamp envelope if present, return raw data."""
    if isinstance(payload, dict) and "data" in payload:
        return payload["data"]
    return payload


def get_resource(session: zenoh.Session, sn: str, path: str):
    """GET a resource via Zenoh queryable."""
    replies = session.get(f"wuji/{sn}/{path}", timeout=5.0)
    for reply in replies:
        return unwrap_envelope(json.loads(bytes(reply.ok.payload)))
    raise RuntimeError(f"GET {path}: no reply")


def main():
    parser = argparse.ArgumentParser(description="Zenoh Bridge Realtime Control Test")
    parser.add_argument("--sn", type=str, default=None, help="Hand sanitized SN (e.g. WUJIHAND_001)")
    parser.add_argument("--duration", type=float, default=10.0, help="Test duration (seconds)")
    parser.add_argument("--rate", type=float, default=1000.0, help="Control loop rate (Hz)")
    args = parser.parse_args()

    session = zenoh.open(zenoh.Config())
    sn = find_hand(session, args.sn)
    zid = acquire_control(session, sn)

    # Subscribe to position and effort feedback
    latest_pos = [None]
    latest_effort = [None]
    sub_pos = session.declare_subscriber(
        f"wuji/{sn}/joint/actual_position",
        lambda sample: latest_pos.__setitem__(0, unwrap_envelope(json.loads(bytes(sample.payload)))),
    )
    sub_effort = session.declare_subscriber(
        f"wuji/{sn}/joint/actual_effort",
        lambda sample: latest_effort.__setitem__(0, unwrap_envelope(json.loads(bytes(sample.payload)))),
    )

    try:
        # Enable all joints
        print("Enabling all joints...")
        set_resource(session, sn, "joint/enabled", [[True] * 4 for _ in range(5)])
        time.sleep(0.5)

        # Read initial position
        print("Reading effort limits...")
        effort_limit = get_resource(session, sn, "joint/effort_limit")
        print(f"  Effort limits: {effort_limit[0]}")

        # Sine wave control loop
        print(f"Running sine wave for {args.duration}s at {args.rate}Hz...")
        print("  F1 (thumb) stays still, F2-F5 do sine on J1/J3/J4")

        period = 1.0 / args.rate
        x = 0.0
        t_start = time.time()

        while (time.time() - t_start) < args.duration:
            y = (1 - math.cos(x)) * 0.8

            target = [
                [0, 0, 0, 0],       # F1 (thumb) - stay still
                [y, 0, y, y],       # F2 (index)
                [y, 0, y, y],       # F3 (middle)
                [y, 0, y, y],       # F4 (ring)
                [y, 0, y, y],       # F5 (pinky)
            ]

            # Fire-and-forget PUT for low-latency target updates
            session.put(f"wuji/{sn}/joint/target_position",
                        json.dumps(target).encode("utf-8"))

            # Print feedback
            if latest_pos[0] is not None:
                actual = latest_pos[0]
                error_j1 = target[1][0] - actual[1][0]
                effort_str = ""
                if latest_effort[0] is not None:
                    effort_f2 = latest_effort[0][1]
                    effort_limit_f2 = effort_limit[1]
                    effort_pct = [e / l * 100 if l != 0 else 0 for e, l in zip(effort_f2, effort_limit_f2)]
                    effort_str = f"  effort%=[{effort_pct[0]:.0f},{effort_pct[2]:.0f},{effort_pct[3]:.0f}]"
                print(f"\r  y={y:.2f}  actual={actual[1][0]:.2f}  err={error_j1:.3f}{effort_str}", end="", flush=True)

            x += math.pi / args.rate
            time.sleep(period)

        print("\n\nSine wave complete.")

    finally:
        # Return to zero and disable
        print("Returning to zero position...")
        session.put(f"wuji/{sn}/joint/target_position",
                    json.dumps([[0] * 4 for _ in range(5)]).encode("utf-8"))
        time.sleep(1.0)

        print("Disabling joints...")
        set_resource(session, sn, "joint/enabled", [[False] * 4 for _ in range(5)])

        sub_pos.undeclare()
        sub_effort.undeclare()
        release_control(session, sn, zid)
        session.close()
        print("Done.")


if __name__ == "__main__":
    main()
