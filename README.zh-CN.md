# WujihandPy：Unified Wuji Hand SDK: C++ Core with Python Bindings

[English](README.md) | 简体中文

WujihandPy 是 [WujihandCpp](wujihandcpp/README.md) 的 Python 绑定，提供面向舞肌灵巧手设备的易用 Python API（底层由 C++ SDK 驱动）。

## 文档

[快速开始](https://docs.wuji.tech/docs/zh/wuji-hand/latest/sdk-user-guide/introduction/)

## 安装

WujihandPy 支持 pip 一键安装：

```bash
pip install wujihandpy
```

对于 Linux 用户，需要额外配置 udev 规则以允许非 root 用户访问 USB 设备，可在终端中输入：

```bash
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="0483", MODE="0666"' |
sudo tee /etc/udev/rules.d/95-wujihand.rules &&
sudo udevadm control --reload-rules &&
sudo udevadm trigger
```

### 常见错误

若安装时报错 `Could not find a version that satisfies the requirement`，请先升级 pip：

```bash
python3 -m pip install --upgrade pip
```

再使用升级后的 pip 重试：

```bash
python3 -m pip install wujihandpy
```

## 支持的 CPU 架构

- x86_64
- ARM64

## 最低系统要求 (Linux)

### glibc 2.28+

使用 glibc 2.28 或更高版本的 Linux 发行版：
- Debian 10+
- Ubuntu 18.10+
- Fedora 29+
- CentOS/RHEL 8+

### Python 3.8+

支持以下 Python 版本：

- Python 3.8-3.14

## 最低系统要求 (Windows)

WujihandPy 目前暂不支持 Windows，我们会尽快推进相关支持。

## 快速开始

### 导入模块

```python
import wujihandpy
import numpy as np
```

### 连接至灵巧手

```python
hand = wujihandpy.Hand()
```

### 读数据

```python
def read_<dataname>(self) -> datatype
def read_<dataname>(self) -> np.ndarray[datatype] # For bulk-read
```

所有可使用的数据见 [API Reference](https://docs.wuji.tech/docs/zh/wuji-hand/latest/sdk-user-guide/api-reference/)。

例如，读取灵巧手的上电运行时间（us）：

```python
time = hand.read_system_time()
```

除整手级数据外，每个关节也有自己的数据；关节级数据名均以 `joint` 作为前缀。

例如，读取第 1 个手指（食指）第 0 个关节的当前位置数据：

```python
position = hand.finger(1).joint(0).read_joint_actual_position()
```

关节角度为 `np.float64` 类型，单位为弧度；零点与正方向与 [URDF 文件](https://github.com/Wuji-Technology-Co-Ltd/wujihand-urdf) 的定义一致。

用一条指令读取多个数据称为**批量读（Bulk-Read）**。

例如，以下指令读取整手所有（20 个）关节的当前位置数据：

```python
positions = hand.read_joint_actual_position()
```

进行批量读时，函数返回包含所有数据的 `np.ndarray[np.float64]`：

```python
>>> print(positions)
[[ 0.975  0.523  0.271 -0.45 ]
 [ 0.382  0.241 -0.003 -0.275]
 [-0.299  0.329  0.067 -0.286]
 [-0.122  0.228  0.315 -0.178]
 [ 0.205  0.087  0.288 -0.149]]
```

`read` 函数会阻塞，直到读取完成。保证当函数返回时，读取一定成功。

### 写数据

写数据拥有类似的 API，但多了一个参数用于传递目标值：

```python
def write_<dataname>(self, datatype)
def write_<dataname>(self, np.ndarray[datatype]) # For bulk-write
```

例如，写入单个关节的目标位置数据：

```python
hand.finger(1).joint(0).write_joint_target_position(0.8)
```

各关节的合法角度范围可通过以下 API 获取：

```python
upper = < Hand / Finger / Joint >.read_joint_upper_limit()
lower = < Hand / Finger / Joint >.read_joint_lower_limit()
```

若写入的角度超出合法范围，会被自动限幅至最高/最低值。

**批量写（Bulk-Write）** 同样可用。例如，为第 1 个手指（食指）的所有关节写入相同的目标位置：

```python
hand.finger(1).write_joint_target_position(0.8)
```

如果每个关节的目标值不同，可以传入一个包含各关节目标值的 `np.ndarray`：

```python
hand.finger(1).write_joint_target_position(
    np.array(
        #   J1    J2    J3    J4
        [0.8,  0.0,  0.8,  0.8],
        dtype=np.float64,
    )
)
```

`write` 函数会阻塞，直到写入完成。保证当函数返回时，写入一定成功。

### 实时控制

默认的读/写方式均带有缓冲池，积攒一段时间数据后才进行传输，最高读/写频率无法超过 100Hz。

对于需要流畅控制关节位置的场景，请使用 [实时控制](https://docs.wuji.tech/docs/zh/wuji-hand/latest/sdk-user-guide/tutorial/#4-%E5%AE%9E%E6%97%B6%E6%8E%A7%E5%88%B6)。



### 异步读/写

读写函数均有对应的异步版本，函数名以 `_async` 作为后缀。

```python
async def read_<dataname>_async(self) -> datatype
async def read_<dataname>_async(self) -> np.ndarray[datatype] # For bulk-read
async def write_<dataname>_async(self, datatype)
async def write_<dataname>_async(self, np.ndarray[datatype])  # For bulk-write
```

异步接口需 `await`；等待期间不阻塞线程/事件循环，返回时保证读/写已成功。

### Unchecked 读/写

如果不关心读/写是否成功，可以使用 Unchecked 版本的读/写函数，函数名以 `_unchecked` 作为后缀。

```python
def read_<dataname>_unchecked(self) -> None
def write_<dataname>_unchecked(self, datatype)
def write_<dataname>_unchecked(self, np.ndarray[datatype])  # For bulk-write
```

Unchecked 函数总是立即返回，不会阻塞，通常用于对实时性要求较高的场景。

### 获取缓存值（Get）

如果希望获取以往读/写的结果，可以使用 `get` 系列函数：

```python
def get_<dataname>(self) -> datatype
def get_<dataname>(self) -> np.ndarray[datatype] # For bulk-read
```

`get` 系列函数同样不会阻塞，它总是立即返回最近一次读取到的数据，无论该数据来自 `read`、`async-read` 还是 `read-unchecked`。

如果尚未请求过该数据，或请求尚未成功，`get` 的返回值是未定义的（通常为 0）。

## 示例代码

所有示例代码均位于 [example](example) 目录下。

## 性能与优化

WujihandPy 在充分保证易用性的同时，尽可能优化了性能与效率。

我们建议优先使用批量读/写以最大限度发挥性能。

对于需要流畅控制关节位置的场景，请务必使用 realtime_controller。

## 许可证

本项目采用 MIT 许可证，详情见 [LICENSE](LICENSE) 文件。
