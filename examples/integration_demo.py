#!/usr/bin/env python3
"""
一代手 + 触觉板 集成示例

展示如何同时使用:
1. wujihandpy Hand（一代手关节数据）
2. wujihandpy TouchBoard（触觉板压力矩阵）
3. wujihandpy Bridge → Zenoh → wuji-sdk（桥接到 SDK 层）

=== 架构 ===

  WujiHand 一代手 (USB CANopen)        tboard 触觉板 (USB CDC)
       ↓                                     ↓
  wujihandpy.Hand                    wujihandpy.TouchBoard
       ↓                                     ↓
  HandBridge (Zenoh pub)             TactileBridge (Zenoh pub)
       ↓                                     ↓
       └──────── Zenoh Network ───────────────┘
                      ↓
               wuji_sdk.SdkManager
                      ↓
              subscribe("tactile")
              get("hand_side")
              ...

=== 运行方式 ===

终端 1: 启动一代手桥接
    wujihandpy-bridge --sn <一代手SN> --pub-rate 100

终端 2: 启动触觉桥接
    wujihandpy-tactile-bridge --sn <tboard_SN> --pub-rate 30

终端 3: SDK 订阅端（本脚本）
    python examples/integration_demo.py sdk-sub

=== 环境要求 ===

    pip install wujihandpy[bridge]
    cd ~/work/wuji-sdk && maturin develop -m crates/sdk-python/Cargo.toml
"""

import sys
import time
import json
import numpy as np


# ============================================================================
# 场景 1: 纯 wujihandpy 本地使用（不需要 Zenoh / wuji-sdk）
# ============================================================================

def demo_local_hand():
    """直接通过 wujihandpy 读取一代手关节数据。"""
    from wujihandpy import Hand

    print("=" * 60)
    print("  场景 1: 一代手本地读取")
    print("=" * 60)

    hand = Hand()  # 自动发现 USB 设备
    print("Connected to hand")

    # 读取关节角度
    for _ in range(5):
        pos = hand.read_joint_actual_position()
        print(f"  Joint positions: {pos[:4]}...")  # 前 4 个关节
        time.sleep(0.1)


def demo_local_touchboard():
    """直接通过 wujihandpy 读取触觉板数据。"""
    from wujihandpy import TouchBoard

    print("=" * 60)
    print("  场景 2: 触觉板本地读取")
    print("=" * 60)

    tb = TouchBoard()
    print(f"Connected! Handedness: {tb.handedness}")

    for i in range(5):
        data = tb.read_tactile()
        print(f"  Frame {i}: shape={data.shape} "
              f"range=[{data.min():.3f}, {data.max():.3f}] "
              f"mean={data.mean():.3f} fps={tb.fps:.0f}")
        time.sleep(0.1)

    raw = tb.read_tactile_raw()
    print(f"  Raw: range=[{raw.min()}, {raw.max()}]")


# ============================================================================
# 场景 2: Bridge 模式（wujihandpy → Zenoh → wuji-sdk）
# ============================================================================

def demo_bridge_hand(sn=None):
    """启动一代手桥接，将数据发布到 Zenoh 网络。

    推荐方式 — 用 CLI（会自动处理 Hand 连接和 SN）:
        wujihandpy-bridge --sn <SN> --pub-rate 100

    本函数展示等效的 Python 调用方式:
    """
    import wujihandpy
    from wujihandpy.bridge import HandBridge

    print("=" * 60)
    print("  场景 3: 一代手 → Zenoh Bridge")
    print("=" * 60)

    print(f"Connecting to Hand (SN: {sn or 'auto'})...")
    hand = wujihandpy.Hand(serial_number=sn)
    actual_sn = hand.get_product_sn() or (sn or "auto")

    bridge = HandBridge(hand=hand, serial_number=actual_sn, pub_rate=100)
    print(f"Starting HandBridge (SN: {actual_sn})...")
    print("Press Ctrl+C to stop")

    bridge.start()
    wait_fn = getattr(bridge, "wait", None)
    if callable(wait_fn):
        try:
            wait_fn()
        except KeyboardInterrupt:
            bridge.stop()
    else:
        import time
        print("Bridge running. Press Ctrl+C to stop.")
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            bridge.stop()


def demo_bridge_tactile(sn=None):
    """启动触觉桥接，将触觉数据发布到 Zenoh 网络。

    在另一个终端运行:
        wujihandpy-tactile-bridge --sn <SN> --pub-rate 30
    或:
        python -c "
        from wujihandpy.bridge import TactileBridge
        bridge = TactileBridge(pub_rate=30)
        bridge.run()
        "
    """
    from wujihandpy.bridge import TactileBridge

    print("=" * 60)
    print("  场景 4: 触觉板 → Zenoh Bridge")
    print("=" * 60)

    bridge = TactileBridge(serial_number=sn, pub_rate=30)
    print(f"Starting TactileBridge (SN: {sn or 'auto'})...")
    print("Press Ctrl+C to stop")
    bridge.run()  # 阻塞


# ============================================================================
# 场景 3: wuji-sdk 订阅端（消费 Zenoh 上的数据）
# ============================================================================

def demo_sdk_subscribe():
    """通过 wuji-sdk 订阅 Zenoh 网络上的设备数据。

    前提: 需要先启动 Bridge (场景 3/4)。
    """
    from wuji_sdk import SdkManager, set_log_level

    print("=" * 60)
    print("  场景 5: wuji-sdk 通过 Zenoh 订阅")
    print("=" * 60)

    set_log_level("info")
    manager = SdkManager.instance()

    # 扫描 Zenoh 网络上的设备
    print("Scanning Zenoh network...")
    devices = manager.scan()
    print(f"Found {len(devices)} device(s):")
    for d in devices:
        print(f"  - SN: {d.sn}, Transport: {d.transport_type}, Address: {d.address}")

    if not devices:
        print("No devices found. Make sure TactileBridge is running.")
        print("Run: wujihandpy-tactile-bridge --pub-rate 30")
        return

    # 优先选择有 tactile 资源的设备（tboard），否则选第一个
    target = None
    for d in devices:
        if "tboard" in d.sn.lower() or d.sn.startswith("tboard"):
            target = d
            break
    if target is None:
        # 如果只有一个设备，用它
        if len(devices) == 1:
            target = devices[0]
        else:
            print("Multiple devices found. Please specify SN:")
            for d in devices:
                print(f"  {d.sn}")
            return

    sn = target.sn
    device = manager.connect(sn=sn, device_name="demo_device")
    print(f"\nConnected to: {device.serial_number}")

    # 列出可用资源
    params = device.params()
    topics = device.topics()
    print(f"Params: {[p.path for p in params]}")
    print(f"Topics: {[t.path for t in topics]}")

    # 订阅触觉数据
    try:
        sub = device.subscribe("tactile")
        print("\nSubscribed to tactile! Waiting for data...")
        for i in range(10):
            frame = sub.recv()
            if frame is not None:
                print(f"  [{i}] data_len={len(frame.data)} seq={frame.header.seq}")
            time.sleep(0.1)
    except Exception as e:
        print(f"Subscribe failed: {e}")

    device.disconnect()
    print("Done!")


# ============================================================================
# 场景 4: 完整集成 — 同时运行 Bridge + SDK 订阅
# ============================================================================

def demo_full_integration():
    """在同一进程中同时运行触觉 Bridge 和 Zenoh 订阅。

    演示完整数据链路: tboard → TouchBoard → TactileBridge → Zenoh → 订阅
    """
    import zenoh

    print("=" * 60)
    print("  场景 6: 完整集成 (Bridge + 本地 Zenoh 订阅)")
    print("=" * 60)

    # Step 1: 连接 TouchBoard
    from wujihandpy import TouchBoard
    print("[1] Connecting to TouchBoard...")
    tb = TouchBoard()
    print(f"    Handedness: {tb.handedness}, FPS: {tb.fps:.0f}")

    # Step 2: 启动 Zenoh session
    print("[2] Opening Zenoh session...")
    session = zenoh.open(zenoh.Config())

    # Step 3: 声明发布者
    sn = "tboard"
    pub_tactile = session.declare_publisher(f"wuji/tboard_{sn}/tactile")
    alive_token = session.liveliness().declare_token(f"wuji/tboard_{sn}/@alive")
    print(f"    Publishing to wuji/tboard_{sn}/tactile")

    # Step 4: 同时订阅（验证端到端）
    received = {"count": 0, "last_data": None}

    def on_sample(sample):
        data = json.loads(bytes(sample.payload))
        received["count"] += 1
        received["last_data"] = data
        if received["count"] <= 3:
            print(f"    [SUB] Received #{received['count']}: "
                  f"data_len={len(data.get('data', []))}")

    sub = session.declare_subscriber(f"wuji/tboard_{sn}/tactile", on_sample)
    print("[3] Subscriber ready")

    # Step 5: 发布循环
    print("[4] Publishing tactile data (5 seconds)...")
    start = time.time()
    pub_count = 0

    while time.time() - start < 5:
        data = tb.get_tactile()
        if data is not None:
            payload = json.dumps({
                "timestamp_us": int(time.time() * 1_000_000),
                "data": data.flatten().tolist()
            })
            pub_tactile.put(payload.encode())
            pub_count += 1
        time.sleep(1.0 / 30)  # 30 Hz

    elapsed = time.time() - start
    print("\n[5] Results:")
    print(f"    Published: {pub_count} frames in {elapsed:.1f}s "
          f"({pub_count / elapsed:.1f} Hz)")
    print(f"    Received:  {received['count']} frames")

    # Cleanup
    sub.undeclare()
    alive_token.undeclare()
    session.close()

    if received["count"] > 0:
        print("\n=== END-TO-END VERIFIED ===")
    else:
        print("\n=== WARNING: No data received via Zenoh ===")


# ============================================================================
# Main
# ============================================================================

DEMOS = {
    "local-hand": ("一代手本地读取", demo_local_hand),
    "local-touch": ("触觉板本地读取", demo_local_touchboard),
    "bridge-hand": ("一代手 Zenoh Bridge", demo_bridge_hand),
    "bridge-touch": ("触觉板 Zenoh Bridge", demo_bridge_tactile),
    "sdk-sub": ("wuji-sdk Zenoh 订阅", demo_sdk_subscribe),
    "full": ("完整集成测试", demo_full_integration),
}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in DEMOS:
        print("Usage: python integration_demo.py <demo>")
        print()
        print("Available demos:")
        for key, (desc, _) in DEMOS.items():
            print(f"  {key:15s}  {desc}")
        print()
        print("Quick start:")
        print("  python integration_demo.py local-touch   # 直接读触觉板")
        print("  python integration_demo.py full           # 端到端集成")
        sys.exit(1)

    name = sys.argv[1]
    desc, func = DEMOS[name]
    print(f"\n🚀 Running: {desc}\n")

    try:
        if name in ("bridge-hand", "bridge-touch"):
            sn = sys.argv[2] if len(sys.argv) > 2 else None
            func(sn)
        else:
            func()
    except KeyboardInterrupt:
        print("\nStopped by user")
    except Exception as e:
        print(f"\nError: {e}")
        import traceback
        traceback.print_exc()
