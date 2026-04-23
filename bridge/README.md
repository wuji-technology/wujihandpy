# Wuji Hand Zenoh Bridge

Expose a WujiHand device over Zenoh so `wuji-sdk` clients can discover and control it transparently across the network.

## Architecture

```text
wuji-sdk client <-> Zenoh network <-> Hand Bridge <-> USB <-> WujiHand hardware
```

The bridge owns the device's exclusive USB connection and registers Zenoh queryables (GET/SET) plus publishers (SUB streams). `wuji-sdk` clients discover the hand via Zenoh liveliness, query `@capability` for the resource list, and then interact with it just like other Wuji devices.

## Implementations

| | Python | C++ |
|---|---|---|
| Path | `bridge/python/` | `bridge/cpp/` |
| Dependencies | eclipse-zenoh, wujihandpy, numpy | zenoh-cpp, wujihandcpp, nlohmann/json |
| Best for | Development, debugging, fast iteration | Production deployment, lower latency |
| Control path | 100 Hz feed loop -> controller | Zenoh callback writes directly to controller |

## Quick Start

### Python Bridge

```bash
cd wujihandpy
source .venv/bin/activate
pip install eclipse-zenoh numpy

# Start the bridge (device over USB; --pub-rate and --side are required)
PYTHONPATH=. python -m bridge.python.hand_zenoh_bridge --pub-rate 1000 --side left

# Full arguments
PYTHONPATH=. python -m bridge.python.hand_zenoh_bridge \
    --sn "DEVICE_SN" \
    --pub-rate 1000 \
    --side left \
    --log-level DEBUG
```

`--side {left,right}` selects which set of joint names is published on
`joint_states` (`left_finger{1..5}_joint{1..4}` or `right_...`). It must match
the URDF loaded downstream (e.g. in Wuji Studio).

`--filter-cutoff <Hz>` (default `5.0`) tunes the internal
`realtime_controller` LowPass cutoff. The filter runs at PDO 1 kHz and
smooths target_position writes from the bridge's 100 Hz feed loop out to the
1 ms PDO tick, so remote clients can send step targets at any rate without
stair-stepping the motors. Lower the cutoff for smoother motion, raise it
(e.g. `10000`) to approximate passthrough if you prefer to pre-filter on the
client.

### C++ Bridge

```bash
cd bridge/cpp
mkdir -p build && cd build
cmake .. && cmake --build . -j$(nproc)

# Start the bridge (--pub-rate is required)
./wujihand_zenoh_bridge --pub-rate 1000 --log-level info

# Full arguments
./wujihand_zenoh_bridge --sn "DEVICE_SN" --pub-rate 1000 --log-level debug
```

## Client Usage

```python
import json
import zenoh

sn = "WUJIHAND_001"


def callback(sample):
    payload = json.loads(bytes(sample.payload).decode("utf-8"))
    print("actual_position:", payload)


session = zenoh.open(zenoh.Config())
zid = str(session.zid())
target = [[0.1, 0.0, 0.1, 0.1] for _ in range(5)]
sub = None
owner_token = session.liveliness().declare_token(f"wuji/{sn}/@control_owner/{zid}")

try:
    # 1. Discover devices via liveliness
    replies = session.liveliness().get("wuji/**")

    # 2. Query capability
    replies = session.get(f"wuji/{sn}/@capability", timeout=5.0)

    # 3. Acquire control (declare the control-owner liveliness token first)
    session.get(
        f"wuji/{sn}/@control",
        payload=f"acquire:{zid}".encode(),
        attachment=zid.encode(),
        timeout=5.0,
    )

    # 4. Read data (GET)
    replies = session.get(f"wuji/{sn}/joint/actual_position", timeout=5.0)

    # 5. Write target position (low-latency fire-and-forget PUT)
    session.put(
        f"wuji/{sn}/joint/target_position",
        json.dumps(target).encode(),
        attachment=zid.encode(),
    )

    # 6. Subscribe to realtime data (rate controlled by --pub-rate)
    sub = session.declare_subscriber(f"wuji/{sn}/joint/actual_position", callback)
finally:
    session.get(
        f"wuji/{sn}/@control",
        payload=f"release:{zid}".encode(),
        attachment=zid.encode(),
        timeout=5.0,
    )
    owner_token.undeclare()
    if sub is not None:
        sub.undeclare()
    session.close()
```

## Resources

### GET Resources

| Path | Type | Description |
|------|------|-------------|
| `input_voltage` | number | Input voltage |
| `temperature` | number | Device temperature |
| `handedness` | integer | Left/right hand |
| `firmware_version` | integer | Firmware version |
| `joint/actual_position` | 5x4 float | Actual joint position (SUB rate controlled by `--pub-rate`) |
| `joint/actual_effort` | 5x4 float | Actual joint effort (SUB rate controlled by `--pub-rate`) |
| `joint/temperature` | 5x4 float | Joint temperature |
| `joint/error_code` | 5x4 int | Joint error code |
| `joint/effort_limit` | 5x4 float | Effort limit (read/write) |
| `joint/upper_limit` | 5x4 float | Joint upper limit |
| `joint/lower_limit` | 5x4 float | Joint lower limit |
| `joint/bus_voltage` | 5x4 float | Joint bus voltage |

### SUB-only Resources

| Path | Type / Schema title | Description |
|------|---------------------|-------------|
| `joint_states` | `sensor_msgs/JointState` — `{name: string[20], position: number[20]}` | Flat row-major projection of `joint/actual_position`. Joint names follow `{side}_finger{1..5}_joint{1..4}`, matching [`wuji-hand-description`](https://github.com/wuji-technology/wuji-hand-description) URDFs. **Published without the timestamp envelope** so downstream consumers (e.g. Wuji Studio's 3D panel) can identify it directly by schema title and drive a URDF visualization. |

### SET Resources

These resources require control ownership.

| Path | Type | Description |
|------|------|-------------|
| `joint/target_position` | 5x4 float | Target position |
| `joint/control_mode` | 5x4 int | Control mode |
| `joint/enabled` | 5x4 bool | Joint enable state |
| `joint/effort_limit` | 5x4 float | Effort limit |
| `joint/reset_error` | 5x4 int | Error reset |

## Data Format

GET/queryable replies always use the resource's original schema. Most SUB streams are wrapped in a timestamped envelope, with one exception called out below.

### SUB Stream Format

By default, SUB resources are published as a timestamped envelope:

```json
{
  "timestamp_us": 1773822692412074,
  "data": [[0.1, 0.2, 0.3, 0.4], "..."]
}
```

**Exception:** `joint_states` is published raw (no envelope) so its schema title remains exactly `sensor_msgs/JointState` for downstream consumers that key on schema name (e.g. Wuji Studio's 3D panel). Ordering for this topic is carried in the standard ROS `header.stamp` field instead of `timestamp_us`.

## Control Protocol

- Acquire: send `acquire:{your_zid}` and receive `granted` or `denied:{current_owner}`
- Release: send `release:{your_zid}` and receive `released`
- Query: send an empty payload and receive the current owner or `none`
- Auto-release on crash: the bridge watches the owner's Zenoh liveliness token and releases control if that process disappears

Control-changing requests must attach the requester identity in the Zenoh attachment. The bridge verifies that:
- `@control` acquire/release uses the same requester in both payload and attachment
- fire-and-forget `joint/target_position` PUT uses the current control owner's requester id in the attachment

## Visualize in Wuji Studio

With the bridge running, you can view the hand in Studio's 3D panel without
touching the control path:

1. Open Wuji Studio. When it discovers this bridge over Zenoh, the topic
   `/wuji/{sn}/joint_states` becomes subscribable (schema
   `sensor_msgs/JointState`).
2. Open a **3D** panel → **Add URDF** → source **filePath** → point to
   `wuji-hand-description/urdf/left.urdf` (or `right.urdf` — must match the
   `--side` you launched the bridge with).
3. The 3D panel auto-subscribes `joint_states` and animates the URDF.

## Tests

```bash
# Unit tests (no hardware required)
cd wujihandpy
source .venv/bin/activate
PYTHONPATH=. python -m pytest tests/test_bridge.py -v
```
