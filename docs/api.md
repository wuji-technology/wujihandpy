# WujihandPy API Reference

WujihandPy 是 WujiHandCpp SDK 的 Python 绑定库，通过 pybind11 提供 Python 接口，支持 USB 通信控制灵巧手设备。

## 目录

- [模块导入](#模块导入)
- [Hand 类](#hand-类)
- [Finger 类](#finger-类)
- [Joint 类](#joint-类)
- [IController 接口](#icontroller-接口)
- [filter 模块](#filter-模块)
- [logging 模块](#logging-模块)
- [异常处理](#异常处理)
- [使用示例](#使用示例)

---

## 模块导入

```python
import wujihandpy
from wujihandpy import Hand, Finger, Joint, IController
from wujihandpy import filter, logging
```

---

## Hand 类

灵巧手主类，用于设备连接和管理。

### 构造函数

```python
Hand(serial_number: str | None = None,
     usb_pid: int = -1,
     usb_vid: int = 0x0483,
     mask: np.ndarray[bool] | None = None) -> None
```

**参数说明：**

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `serial_number` | `str \| None` | `None` | 设备序列号，指定时连接特定设备 |
| `usb_pid` | `int` | `-1` | USB 产品 ID，-1 表示任意 |
| `usb_vid` | `int` | `0x0483` | USB 厂商 ID，默认 0x0483 |
| `mask` | `np.ndarray[bool] \| None` | `None` | 5x4 布尔数组，指定控制哪些关节 |

**USB 设备信息：** VID=0x0483, PID=0x7530

**示例：**

```python
# 自动连接第一个设备
hand = Hand()

# 连接特定设备
hand = Hand(serial_number="WujiHand_001")

# 只控制拇指和食指
mask = np.zeros((5, 4), dtype=bool)
mask[0, :] = True  # 拇指
mask[1, :] = True  # 食指
hand = Hand(mask=mask)
```

### Hand 属性操作方法

每个数据字段提供三种操作模式：

- **同步读/写**: `read_<name>()`, `write_<name>(value)`
- **异步读/写**: `read_<name>_async()`, `write_<name>_async(value)`
- **非检查读/写**: `read_<name>_unchecked()`, `write_<name>_unchecked(value)` - 不等待完成
- **缓存获取**: `get_<name>()` - 非阻塞获取缓存值

#### Hand 只读属性

| 属性名 | 类型 | 说明 |
|--------|------|------|
| `read_handedness()` / `get_handedness()` | `uint8` | 手性 (左/右手) |
| `read_firmware_version()` / `get_firmware_version()` | `uint32` | 固件版本 |
| `read_firmware_date()` / `get_firmware_date()` | `uint32` | 固件日期 |
| `read_system_time()` / `get_system_time()` | `uint32` | 系统时间 |
| `read_temperature()` / `get_temperature()` | `float32` | 温度 (°C) |
| `read_input_voltage()` / `get_input_voltage()` | `float32` | 输入电压 (V) |

#### Hand 关节级属性 (5x4 数组)

| 属性名 | 类型 | 说明 |
|--------|------|------|
| `read_joint_firmware_version()` / `get_joint_firmware_version()` | `NDArray[uint32]` | 各关节固件版本 |
| `read_joint_firmware_date()` / `get_joint_firmware_date()` | `NDArray[uint32]` | 各关节固件日期 |
| `read_joint_actual_position()` / `get_joint_actual_position()` | `NDArray[float64]` | 各关节实际位置 |
| `read_joint_bus_voltage()` / `get_joint_bus_voltage()` | `NDArray[float32]` | 各关节总线电压 |
| `read_joint_temperature()` / `get_joint_temperature()` | `NDArray[float32]` | 各关节温度 |
| `read_joint_error_code()` / `get_joint_error_code()` | `NDArray[uint32]` | 各关节错误码 |
| `read_joint_effort_limit()` / `get_joint_effort_limit()` | `NDArray[float64]` | 各关节力矩限制 (A) |
| `read_joint_current_limit()` / `get_joint_current_limit()` | `NDArray[float64]` | 各关节电流限制 (A) [已废弃] |
| `read_joint_upper_limit()` / `get_joint_upper_limit()` | `NDArray[float64]` | 各关节上限位置 |
| `read_joint_lower_limit()` / `get_joint_lower_limit()` | `NDArray[float64]` | 各关节下限位置 |

#### Hand 关节级写入属性

| 属性名 | 类型 | 说明 |
|--------|------|------|
| `write_joint_target_position(value, timeout)` | `float64` | 设置目标位置 |
| `write_joint_control_mode(value, timeout)` | `uint16` | 设置控制模式 |
| `write_joint_enabled(value, timeout)` | `bool` | 使能关节 |
| `write_joint_effort_limit(value, timeout)` | `float64` | 设置力矩限制 (A) |
| `write_joint_current_limit(value, timeout)` | `float64` | 设置电流限制 (A) [已废弃] |
| `write_joint_sin_level(value, timeout)` | `uint16` | 设置正弦波等级 |
| `write_joint_reset_error(value, timeout)` | `uint16` | 重置错误 |

**注意：** 关节级写入属性支持单值或数组输入

```python
# 单关节控制
hand.finger(0).joint(0).write_joint_target_position(10.0)

# 整手控制 - 使用数组
positions = np.full((5, 4), 10.0)
hand.write_joint_target_position(positions)

# 单指控制 - 使用数组
finger_positions = np.full(4, 10.0)
hand.finger(0).write_joint_target_position(finger_positions)
```

### Hand 专用方法

| 方法 | 说明 |
|------|------|
| `finger(index) -> Finger` | 获取指定手指 (0-4) |
| `realtime_controller(enable_upstream, filter) -> IController` | 创建实时控制器 |
| `start_latency_test() -> None` | 开始延迟测试 |
| `stop_latency_test() -> None` | 停止延迟测试 |
| `get_product_sn() -> str` | 获取产品序列号 |

### Hand 原始 SDO 操作 (调试用)

| 方法 | 说明 |
|------|------|
| `raw_sdo_read(finger_id, joint_id, index, sub_index, timeout) -> bytes` | 原始 SDO 读 |
| `raw_sdo_write(finger_id, joint_id, index, sub_index, data, timeout) -> None` | 原始 SDO 写 |

---

## Finger 类

手指类，包含 4 个关节。

### 构造函数

通过 `Hand.finger(index)` 获取，不直接构造。

### Finger 方法

| 方法 | 说明 |
|------|------|
| `joint(index) -> Joint` | 获取指定关节 (0-3) |

### Finger 属性操作

Finger 类提供与 Hand 类相同的关节级属性操作（5x4 数组变为 1x4 数组）：

| 属性名 | 类型 | 说明 |
|--------|------|------|
| `read_joint_actual_position()` / `get_joint_actual_position()` | `NDArray[float64]` | 4 个关节位置 |
| `read_joint_firmware_version()` / `get_joint_firmware_version()` | `NDArray[uint32]` | 4 个关节固件版本 |
| `read_joint_firmware_date()` / `get_joint_firmware_date()` | `NDArray[uint32]` | 4 个关节固件日期 |
| `read_joint_bus_voltage()` / `get_joint_bus_voltage()` | `NDArray[float32]` | 4 个关节总线电压 |
| `read_joint_temperature()` / `get_joint_temperature()` | `NDArray[float32]` | 4 个关节温度 |
| `read_joint_error_code()` / `get_joint_error_code()` | `NDArray[uint32]` | 4 个关节错误码 |
| `read_joint_effort_limit()` / `get_joint_effort_limit()` | `NDArray[float64]` | 4 个关节力矩限制 |
| `read_joint_current_limit()` / `get_joint_current_limit()` | `NDArray[float64]` | 4 个关节电流限制 [已废弃] |
| `read_joint_upper_limit()` / `get_joint_upper_limit()` | `NDArray[float64]` | 4 个关节上限位置 |
| `read_joint_lower_limit()` / `get_joint_lower_limit()` | `NDArray[float64]` | 4 个关节下限位置 |
| `write_joint_target_position(value_array)` | `NDArray[float64]` | 设置 4 个关节目标位置 |
| `write_joint_control_mode(value_array)` | `NDArray[uint16]` | 设置 4 个关节控制模式 |
| `write_joint_enabled(value_array)` | `NDArray[bool]` | 使能 4 个关节 |
| `write_joint_effort_limit(value_array)` | `NDArray[float64]` | 设置 4 个关节力矩限制 |
| `write_joint_current_limit(value_array)` | `NDArray[float64]` | 设置 4 个关节电流限制 [已废弃] |
| `write_joint_sin_level(value_array)` | `NDArray[uint16]` | 设置 4 个关节正弦波等级 |
| `write_joint_reset_error(value_array)` | `NDArray[uint16]` | 重置 4 个关节错误 |

---

## Joint 类

关节类，代表单个电机关节。

### 构造函数

通过 `Finger.joint(index)` 获取，不直接构造。

### Joint 属性操作

Joint 类提供单个值的属性操作：

| 属性名 | 类型 | 说明 |
|--------|------|------|
| `read_joint_actual_position()` / `get_joint_actual_position()` | `float64` | 关节实际位置 |
| `read_joint_firmware_version()` / `get_joint_firmware_version()` | `uint32` | 关节固件版本 |
| `read_joint_firmware_date()` / `get_joint_firmware_date()` | `uint32` | 关节固件日期 |
| `read_joint_bus_voltage()` / `get_joint_bus_voltage()` | `float32` | 关节总线电压 |
| `read_joint_temperature()` / `get_joint_temperature()` | `float32` | 关节温度 |
| `read_joint_error_code()` / `get_joint_error_code()` | `uint32` | 关节错误码 |
| `read_joint_effort_limit()` / `get_joint_effort_limit()` | `float64` | 关节力矩限制 (A) |
| `read_joint_current_limit()` / `get_joint_current_limit()` | `float64` | 关节电流限制 (A) [已废弃] |
| `read_joint_upper_limit()` / `get_joint_upper_limit()` | `float64` | 关节上限位置 |
| `read_joint_lower_limit()` / `get_joint_lower_limit()` | `float64` | 关节下限位置 |
| `write_joint_target_position(value)` | `float64` | 设置关节目标位置 |
| `write_joint_control_mode(value)` | `uint16` | 设置关节控制模式 |
| `write_joint_enabled(value)` | `bool` | 使能关节 |
| `write_joint_effort_limit(value)` | `float64` | 设置关节力矩限制 (A) |
| `write_joint_current_limit(value)` | `float64` | 设置关节电流限制 (A) [已废弃] |
| `write_joint_sin_level(value)` | `uint16` | 设置正弦波等级 |
| `write_joint_reset_error(value)` | `uint16` | 重置关节错误 |

**注意：** J1 关节（拇指除外）位置值会被自动反转。

---

## IController 接口

实时控制器接口，用于高速循环控制（可达 1kHz）。

### 获取方式

```python
controller = hand.realtime_controller(enable_upstream=True, filter=filter.LowPass(10.0))
```

### IController 方法

| 方法 | 说明 |
|------|------|
| `__enter__() -> IController` | 上下文管理器入口 |
| `__exit__(exc_type, exc_val, exc_tb) -> None` | 上下文管理器退出 |
| `close() -> None` | 关闭控制器 |
| `get_joint_actual_position() -> NDArray[float64]` | 获取所有关节实际位置 (5x4) |
| `get_joint_actual_effort() -> NDArray[float64]` | 获取所有关节实际力矩 (5x4) |
| `set_joint_target_position(value_array) -> None` | 设置所有关节目标位置 (5x4) |

**使用示例：**

```python
import wujihandpy as wh
import numpy as np
import asyncio

# 创建实时控制器
with wh.Hand().realtime_controller(
    enable_upstream=True,
    filter=wh.filter.LowPass(10.0)
) as controller:
    # 循环控制
    while True:
        # 获取当前位置
        positions = controller.get_joint_actual_position()

        # 设置新的目标位置
        target = np.full((5, 4), 0.0)
        controller.set_joint_target_position(target)

        await asyncio.sleep(0.001)  # 1ms 周期
```

---

## filter 模块

滤波器模块，用于实时控制器。

### LowPass 类

低通滤波器，用于平滑控制信号。

```python
filter.LowPass(cutoff_freq: float = 10.0) -> IFilter
```

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `cutoff_freq` | `float` | `10.0` | 截止频率 (Hz) |

**使用示例：**

```python
from wujihandpy import filter

# 创建低通滤波器
lp_filter = filter.LowPass(cutoff_freq=5.0)

# 用于实时控制器
controller = hand.realtime_controller(enable_upstream=True, filter=lp_filter)
```

---

## logging 模块

日志模块，用于配置 SDK 日志输出。

### Level 枚举

```python
from wujihandpy.logging import Level

Level.TRACE   # 跟踪
Level.DEBUG   # 调试
Level.INFO    # 信息
Level.WARN    # 警告
Level.ERROR   # 错误
Level.CRITICAL # 严重
Level.OFF     # 关闭
```

### 日志配置函数

| 函数 | 说明 |
|------|------|
| `set_log_to_console(value: bool) -> None` | 启用/禁用控制台日志 |
| `set_log_to_file(value: bool) -> None` | 启用/禁用文件日志 |
| `set_log_level(value: Level) -> None` | 设置日志级别 |
| `set_log_path(value: str) -> None` | 设置日志文件路径 |
| `flush() -> None` | 刷新日志 |

**使用示例：**

```python
from wujihandpy import logging

# 配置日志
logging.set_log_to_console(True)
logging.set_log_to_file(True)
logging.set_log_level(logging.Level.DEBUG)
logging.set_log_path("./wujihandpy.log")
```

---

## 异常处理

### TimeoutError

当 SDO 操作超时时抛出。

```python
import wujihandpy as wh

try:
    hand.read_temperature()
except wh.TimeoutError as e:
    print(f"操作超时: {e}")
```

---

## 使用示例

### 基本连接和读取

```python
import wujihandpy as wh
import numpy as np

# 连接设备
hand = wh.Hand()

# 读取设备信息
print(f"固件版本: {hand.read_firmware_version()}")
print(f"温度: {hand.read_temperature()}")

# 读取所有关节位置
positions = hand.read_joint_actual_position()
print(f"关节位置形状: {positions.shape}")  # (5, 4)

# 关闭连接 (上下文管理器自动处理)
```

### 运动控制

```python
import wujihandpy as wh
import numpy as np

hand = wh.Hand()

# 启用所有关节
hand.write_joint_enabled(True)

# 设置目标位置
positions = np.random.uniform(-20, 20, (5, 4))
hand.write_joint_target_position(positions)

# 等待运动完成 (同步)
hand.read_joint_actual_position()  # 阻塞读取

# 异步写入示例
async def move_hand():
    await hand.write_joint_target_position_async(positions)

import asyncio
asyncio.run(move_hand())
```

### 分级访问

```python
import wujihandpy as wh

hand = wh.Hand()

# 访问单个手指
thumb = hand.finger(0)  # 拇指
index_finger = hand.finger(1)  # 食指

# 访问单个关节
thumb_mcp = thumb.joint(0)  # 拇指 MCP 关节

# 读取关节位置
pos = thumb_mcp.read_joint_actual_position()
print(f"拇指 MCP 关节位置: {pos}")

# 设置关节目标位置
thumb_mcp.write_joint_target_position(15.0)
```

### 使用实时控制器

```python
import wujihandpy as wh
from wujihandpy import filter
import numpy as np

hand = wh.Hand()

# 创建实时控制器 (1kHz 控制周期)
with hand.realtime_controller(
    enable_upstream=True,
    filter=filter.LowPass(10.0)
) as controller:
    # 设置初始目标
    target = np.zeros((5, 4))
    controller.set_joint_target_position(target)

    # 实时读取和写入
    for _ in range(1000):
        positions = controller.get_joint_actual_position()
        # 计算新的目标...
        controller.set_joint_target_position(target)
```

---

## 设备层次结构

```
Hand (灵巧手)
├── finger(0)  # 拇指
│   └── joint(0-3)  # 4个关节: MCP, DIP, PIP, IP
├── finger(1)  # 食指
│   └── joint(0-3)
├── finger(2)  # 中指
│   └── joint(0-3)
├── finger(3)  # 无名指
│   └── joint(0-3)
└── finger(4)  # 小指
    └── joint(0-3)
```

---

## API 操作模式说明

| 模式 | 读操作 | 写操作 | 特点 |
|------|--------|--------|------|
| **同步** | `read_<name>(timeout)` | `write_<name>(value, timeout)` | 阻塞等待操作完成，默认超时 500ms |
| **异步** | `read_<name>_async(timeout)` | `write_<name>_async(value, timeout)` | 返回 `Awaitable`，非阻塞 |
| **非检查** | `read_<name>_unchecked(timeout)` | `write_<name>_unchecked(value, timeout)` | 发送后立即返回，不等待完成 |
| **缓存获取** | `get_<name>()` | - | 直接返回缓存值，非阻塞 |

**推荐使用场景：**

- 一次性操作：使用同步模式
- 批量操作：使用异步模式并发执行
- 实时控制循环：使用非检查模式 + `IController`
- 状态查询：使用缓存获取模式
