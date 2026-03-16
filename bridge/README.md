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
6. Starts realtime controller (PDO 1kHz + LowPass 5Hz) for smooth motion
7. Publishes `joint/actual_position` and `joint/actual_effort` at configurable rate (default 50 Hz)

wuji-sdk clients discover the hand through Zenoh and interact
with it identically to other devices (gloves, etc).

## Exposed Resources

### GET (Read-Only)
- `input_voltage` - Supply voltage (float)
- `temperature` - Board temperature (float)
- `handedness` - Left/right hand (int)
- `firmware_version` - Firmware version (int)
- `joint/actual_position` - 5x4 joint positions
- `joint/actual_effort` - 5x4 joint efforts (requires firmware >= 1.2.0)
- `joint/temperature` - 5x4 motor temperatures
- `joint/error_code` - 5x4 error codes
- `joint/effort_limit` - 5x4 effort limits (also SET)
- `joint/upper_limit` - 5x4 upper position limits
- `joint/lower_limit` - 5x4 lower position limits

### SET (Write, Requires Control)
- `joint/control_mode` - 5x4 joint control modes
- `joint/enabled` - 5x4 enable/disable states
- `joint/target_position` - 5x4 target positions
- `joint/effort_limit` - 5x4 effort limits

### SUB (Continuous Stream)
- `joint/actual_position` - Published at `--pub-rate` Hz
- `joint/actual_effort` - Published at `--pub-rate` Hz

## Testing

```bash
# Unit tests (no hardware needed)
python -m pytest tests/test_bridge.py -v

# E2E test (hand must be connected) - see inline test in docs/plans/
```

## Known Limitations

- `get_product_sn()` requires firmware >= 3.1.0-D (falls back to generated SN)
- Realtime controller uses internal PDO 1kHz with LowPass 5Hz filter (not configurable via Zenoh)
- JSON serialization adds latency vs native postcard format
