# wujihandpy

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE) [![Release](https://img.shields.io/github/v/release/wuji-technology/wujihandpy)](https://github.com/wuji-technology/wujihandpy/releases)

Wuji Hand SDK: C++ core with Python bindings, for controlling and communicating with Wuji Hand. WujihandPy is the Python binding of [WujihandCpp](wujihandcpp/README.md), providing an easy-to-use Python API for Wujihand dexterous-hand devices. Supports synchronous, asynchronous, unchecked operations and real-time control.

## Table of Contents

- [Repository Structure](#repository-structure)
- [Usage](#usage)
  - [Prerequisites](#prerequisites)
  - [Installation](#installation)
  - [Running](#running)
- [Troubleshooting](#troubleshooting)
- [Appendix](#appendix)
- [Contact](#contact)

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

## Usage

### Prerequisites

**Supported CPU Architectures:**

- x86_64
- ARM64

**Minimum System Requirements (Linux):**

- glibc 2.28+ (Debian 10+, Ubuntu 18.10+, Fedora 29+, CentOS/RHEL 8+)
- Python 3.8-3.14

**Minimum System Requirements (Windows):**

WujihandPy does not support Windows yet; we will work to add support as soon as possible.

### Installation

WujihandPy supports one-line installation via pip:

```bash
pip install wujihandpy
```

For Linux users, you need to configure udev rules to allow non-root users to access USB devices:

```bash
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="0483", MODE="0666"' | \
sudo tee /etc/udev/rules.d/95-wujihand.rules && \
sudo udevadm control --reload-rules && \
sudo udevadm trigger
```

### Running

#### Import Modules

```python
import wujihandpy
import numpy as np
```

#### Connect to the Hand

```python
hand = wujihandpy.Hand()
```

#### Read Data

```python
def read_<dataname>(self) -> datatype
def read_<dataname>(self) -> np.ndarray[datatype]  # For bulk-read
```

All available data can be found in the [API Reference](https://docs.wuji.tech/docs/en/wuji-hand/latest/sdk-user-guide/api-reference/).

For example, read the hand's powered-on running time (us):

```python
time = hand.read_system_time()
```

Besides hand-level data, each joint also has its own data; joint-level data names all use `joint` as the prefix.

For example, read the current position of joint 0 on finger 1 (index finger):

```python
position = hand.finger(1).joint(0).read_joint_actual_position()
```

Joint angles are of type `np.float64` in radians. The zero point and positive direction follow the definitions in the [URDF files](https://github.com/wuji-technology/wuji-hand-description).

Reading multiple data items with a single command is called **Bulk-Read**.

For example, the following reads the current position of all (20) joints on the hand:

```python
positions = hand.read_joint_actual_position()
```

For bulk reads, the function returns an `np.ndarray[np.float64]` containing all values:

```python
>>> print(positions)
[[ 0.975  0.523  0.271 -0.45 ]
 [ 0.382  0.241 -0.003 -0.275]
 [-0.299  0.329  0.067 -0.286]
 [-0.122  0.228  0.315 -0.178]
 [ 0.205  0.087  0.288 -0.149]]
```

`read` blocks until the operation completes. When the function returns, the read is guaranteed to have succeeded.

#### Write Data

The write API is similar, but takes an extra parameter for the target value:

```python
def write_<dataname>(self, datatype)
def write_<dataname>(self, np.ndarray[datatype])  # For bulk-write
```

For example, write a target position to a single joint:

```python
hand.finger(1).joint(0).write_joint_target_position(0.8)
```

Valid angle limits for each joint can be obtained via:

```python
upper = <Hand / Finger / Joint>.read_joint_upper_limit()
lower = <Hand / Finger / Joint>.read_joint_lower_limit()
```

If the written angle is outside the valid range, it will be automatically clamped to the upper/lower limit.

**Bulk-Write** is also supported. For example, write the same target position to all joints of finger 1 (index finger):

```python
hand.finger(1).write_joint_target_position(0.8)
```

If each joint has a different target, pass an `np.ndarray` containing the target values for each joint:

```python
hand.finger(1).write_joint_target_position(
    np.array(
        #   J1    J2    J3    J4
        [0.8,  0.0,  0.8,  0.8],
        dtype=np.float64,
    )
)
```

`write` blocks until the operation completes. When the function returns, the write is guaranteed to have succeeded.

#### Realtime Control

By default, both reads and writes use a buffer pool: data is accumulated for a while before being transmitted, so the maximum read/write frequency cannot exceed 100 Hz.

For scenarios that require smooth joint position control, use [Realtime Control](https://docs.wuji.tech/docs/en/wuji-hand/latest/sdk-user-guide/tutorial/#4-real-time-control).

#### Asynchronous Read/Write

All read/write functions have asynchronous versions, with an `_async` suffix.

```python
async def read_<dataname>_async(self) -> datatype
async def read_<dataname>_async(self) -> np.ndarray[datatype]  # For bulk-read
async def write_<dataname>_async(self, datatype)
async def write_<dataname>_async(self, np.ndarray[datatype])   # For bulk-write
```

Asynchronous APIs must be awaited. The thread/event loop is not blocked while waiting, and when the call returns the read/write is guaranteed to have succeeded.

#### Unchecked Read/Write

If you do not care whether a read/write succeeds, you can use the Unchecked versions, with an `_unchecked` suffix.

```python
def read_<dataname>_unchecked(self) -> None
def write_<dataname>_unchecked(self, datatype)
def write_<dataname>_unchecked(self, np.ndarray[datatype])  # For bulk-write
```

Unchecked functions always return immediately without blocking, and are typically used in latency-sensitive scenarios.

#### Get Cached Values

If you want to retrieve results from previous reads/writes, use the `get` family of functions:

```python
def get_<dataname>(self) -> datatype
def get_<dataname>(self) -> np.ndarray[datatype]  # For bulk-read
```

`get` functions also never block. They always return the most recently read value, regardless of whether it came from `read`, `async-read`, or `read-unchecked`.

If the data has never been requested, or the request has not succeeded yet, the return value of `get` is undefined (usually 0).

#### Examples

All example code is located in the [example](example) directory.

## Troubleshooting

1. **Could not find a version that satisfies the requirement**

   If you see this error during installation, upgrade pip first:

   ```bash
   python3 -m pip install --upgrade pip
   ```

   Then retry with the upgraded pip:

   ```bash
   python3 -m pip install wujihandpy
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
