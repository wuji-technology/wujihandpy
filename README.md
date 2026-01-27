# wujihandpy

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE) [![Release](https://img.shields.io/github/v/release/wuji-technology/wujihandpy)](https://github.com/wuji-technology/wujihandpy/releases)

Wuji Hand SDK: C++ core with Python bindings, for controlling and communicating with Wuji Hand. WujihandPy is the Python binding of [WujihandCpp](wujihandcpp/README.md), providing an easy-to-use Python API for Wujihand dexterous-hand devices. Supports synchronous, asynchronous, unchecked operations and real-time control.

**Get started with [Quick Start](#quick-start). For detailed documentation, please refer to [SDK Tutorial](https://docs.wuji.tech/docs/en/wuji-hand/latest/sdk-user-guide/introduction) on Wuji Docs Center.**

## Repository Structure

```text
├── src/
│   ├── wujihandpy/
│   │   ├── __init__.py
│   │   └── _core/
│   ├── main.cpp
│   └── *.hpp
├── example/
│   ├── 1.read.py
│   ├── 2.write.py
│   ├── 3.realtime.py
│   └── 4.async.py
├── wujihandcpp/
│   ├── include/
│   │   └── wujihandcpp/
│   ├── src/
│   └── tests/
├── .github/
│   └── workflows/
├── pyproject.toml
├── CMakeLists.txt
└── README.md
```

### Directory Description

| Directory | Description |
|-----------|-------------|
| `src/` | Python binding source code and C++ headers |
| `src/wujihandpy/` | Python package with type stubs |
| `example/` | Usage examples for read, write, realtime, and async operations |
| `wujihandcpp/` | Underlying C++ SDK implementation |
| `wujihandcpp/include/` | C++ header files |
| `wujihandcpp/src/` | C++ source files |
| `.github/workflows/` | CI/CD automation |

## Quick Start

### Installation

```bash
pip install wujihandpy
```

Linux USB permission:

```bash
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="0483", MODE="0666"' | \
sudo tee /etc/udev/rules.d/95-wujihand.rules && \
sudo udevadm control --reload-rules && sudo udevadm trigger
```

### Basic Usage

```python
import wujihandpy

hand = wujihandpy.Hand()

# Read all joint positions
positions = hand.read_joint_actual_position()

# Write target position to a joint
hand.finger(1).joint(0).write_joint_target_position(0.8)
```

## Appendix

### Performance and Optimization

While ensuring usability, WujihandPy has been optimized for performance and efficiency as much as possible.

We recommend prioritizing bulk read/write to maximize performance.

For scenarios that require smooth joint position control, be sure to use `realtime_controller`.

### References

- **Documentation**: [Quick Start](https://docs.wuji.tech/docs/en/wuji-hand/latest/sdk-user-guide/introduction/)
- **API Reference**: [API Reference](https://docs.wuji.tech/docs/en/wuji-hand/latest/sdk-user-guide/api-reference/)
- **URDF Files**: [wuji-hand-description](https://github.com/wuji-technology/wuji-hand-description)

## Contact

For any questions, please contact [support@wuji.tech](mailto:support@wuji.tech).
