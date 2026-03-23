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

# Start the bridge (device must be connected over USB, --pub-rate is required)
PYTHONPATH=. python -m bridge.python.hand_zenoh_bridge --pub-rate 1000

# Full arguments
PYTHONPATH=. python -m bridge.python.hand_zenoh_bridge \
    --sn "DEVICE_SN" \
    --pub-rate 1000 \
    --log-level DEBUG
```

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

GET/queryable replies always use the resource's original schema. Only SUB streams are wrapped in a timestamped envelope.

### SUB Stream Format

SUB resources are published as:

```json
{
  "timestamp_us": 1773822692412074,
  "data": [[0.1, 0.2, 0.3, 0.4], "..."]
}
```

## Control Protocol

- Acquire: send `acquire:{your_zid}` and receive `granted` or `denied:{current_owner}`
- Release: send `release:{your_zid}` and receive `released`
- Query: send an empty payload and receive the current owner or `none`
- Auto-release on crash: the bridge watches the owner's Zenoh liveliness token and releases control if that process disappears

Control-changing requests must attach the requester identity in the Zenoh attachment. The bridge verifies that:
- `@control` acquire/release uses the same requester in both payload and attachment
- fire-and-forget `joint/target_position` PUT uses the current control owner's requester id in the attachment

## Tests

```bash
# Unit tests (no hardware required)
cd wujihandpy
source .venv/bin/activate
PYTHONPATH=. python -m pytest tests/test_bridge.py -v
```
