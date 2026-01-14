# WujihandPy 项目结构文档

## 项目概述

WujihandPy 是 WujiHandCpp SDK 的 Python 绑定库，通过 pybind11 提供 Python 接口，支持 USB 通信控制灵巧手设备。

- **语言**: Python 3.8-3.14 + C++20
- **平台**: Linux (x86_64, ARM64)
- **发布方式**: PyPI (`pip install wujihandpy`)
- **底层库**: [WujihandCpp](https://github.com/wuji-technology/wujihandcpp)

---

## 目录结构

```
wujihandpy/
├── src/                          # Python 绑定层和 C++ 头文件
│   ├── wujihandpy/               # Python 包
│   │   ├── __init__.py           # 包入口，导出公开 API
│   │   └── _core/                # 编译后的扩展模块和类型存根
│   │       ├── __init__.pyi      # 主 API 类型定义
│   │       ├── filter.pyi        # 滤波器模块类型定义
│   │       └── logging.pyi       # 日志模块类型定义
│   ├── main.cpp                  # pybind11 模块入口
│   ├── wrapper.hpp               # 核心 Wrapper<T> 模板
│   ├── controller.hpp            # 实时控制器绑定
│   ├── filter.hpp                # 滤波器绑定
│   └── logging.hpp               # 日志 API 绑定
│
├── wujihandcpp/                  # C++ SDK 子模块 (git submodule)
│   ├── include/wujihandcpp/      # 公开 API 头文件
│   │   ├── device/               # 设备抽象层
│   │   │   ├── hand.hpp          # Hand 类定义
│   │   │   ├── finger.hpp        # Finger 类定义
│   │   │   ├── joint.hpp         # Joint 类定义
│   │   │   ├── controller.hpp    # 控制器接口
│   │   │   ├── data_operator.hpp # 统一读写接口模板
│   │   │   ├── data_tuple.hpp    # 数据元组
│   │   │   ├── latch.hpp         # Latch 同步原语
│   │   │   └── helper.hpp        # 辅助函数
│   │   ├── data/                 # 数据类型定义
│   │   │   ├── hand.hpp          # Hand 数据结构
│   │   │   ├── joint.hpp         # Joint 数据结构
│   │   │   └── helper.hpp        # 数据辅助
│   │   ├── filter/               # 滤波器
│   │   │   └── low_pass.hpp      # 低通滤波器
│   │   ├── protocol/             # 协议处理
│   │   │   ├── handler.hpp       # 协议处理器
│   │   │   ├── frame_builder.hpp # 帧构建器
│   │   │   ├── protocol.hpp      # 协议实现
│   │   │   └── latency_tester.hpp # 延迟测试
│   │   └── utility/              # 工具类
│   │       ├── logging.hpp       # 日志系统
│   │       ├── api.hpp           # API 常量
│   │       ├── ring_buffer.hpp   # 环形缓冲区
│   │       └── singleton.hpp     # 单例模式
│   │
│   ├── src/                      # 实现文件
│   │   ├── device/               # 设备实现
│   │   │   └── latch.cpp         # Latch 同步实现
│   │   ├── protocol/             # 协议实现
│   │   │   ├── handler.cpp       # 协议处理器实现
│   │   │   └── latency_tester.cpp
│   │   ├── transport/            # 传输层
│   │   │   └── usb.cpp           # USB 传输实现 (libusb-1.0)
│   │   └── utility/              # 工具实现
│   │       └── logging.cpp       # 日志实现
│   │
│   └── tests/                    # C++ 测试
│       ├── device/latch_test.cpp # Latch 同步测试
│       └── cpp11_compat/         # C++11 兼容性测试
│
├── example/                      # 使用示例
│   ├── 1.read.py                 # 基本读取操作
│   ├── 2.write.py                # 写入操作
│   ├── 3.realtime.py             # 实时控制
│   └── 4.async.py                # 异步操作
│
├── docs/                         # 文档
│   ├── api.md                    # API 参考文档
│   └── architecture.md           # 本文档
│
├── .github/workflows/            # CI/CD 配置
│   └── *.yml                     # GitHub Actions 工作流
│
├── pyproject.toml                # Python 项目配置
├── CMakeLists.txt                # CMake 构建配置
├── README.md                     # 项目说明
├── CHANGELOG.md                  # 更新日志
└── LICENSE                       # MIT 许可证
```

---

## 三层架构

```
┌─────────────────────────────────────────────────────────────┐
│                      Python 层                               │
│                    (src/wujihandpy/)                         │
│  Hand, Finger, Joint, IController, filter, logging           │
└────────────────────────┬────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────┐
│                   pybind11 绑定层                            │
│                        (src/)                                │
│  wrapper.hpp: Wrapper<T> 模板，管理 GIL 和生命周期            │
│  main.cpp: 模块入口，异常转换 (TimeoutError)                  │
│  controller.hpp: 实时控制器绑定                              │
│  filter.hpp: 滤波器绑定                                      │
│  logging.hpp: 日志 API 绑定                                  │
└────────────────────────┬────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────┐
│                    C++ SDK 层                                │
│                  (wujihandcpp/)                              │
│  device/: Hand, Finger, Joint, Controller                   │
│  protocol/: CANopen 协议处理                                 │
│  transport/: USB 传输 (libusb-1.0)                           │
│  filter/: 低通滤波器                                         │
│  utility/: 日志、工具函数                                    │
└─────────────────────────────────────────────────────────────┘
```

---

## 模块详解

### 1. Python 层 (src/wujihandpy/)

**职责**: 提供 Pythonic API 接口

| 文件 | 职责 |
|------|------|
| `__init__.py` | 包入口，从 `_core` 导入并导出 `Hand, Finger, Joint, IController, filter, logging` |
| `_core/__init__.pyi` | 主模块类型存根，包含 Hand/Finger/Joint/IController 的完整类型定义 |
| `_core/filter.pyi` | 滤波器模块类型定义 |
| `_core/logging.pyi` | 日志模块类型定义 |

**导出 API**:
```python
from wujihandpy import Hand, Finger, Joint, IController
from wujihandpy import filter, logging
```

---

### 2. pybind11 绑定层 (src/)

**职责**: C++ 与 Python 之间的桥接

| 文件 | 职责 |
|------|------|
| `main.cpp` | pybind11 模块入口，注册所有 Python API，处理异常转换 |
| `wrapper.hpp` | 核心 Wrapper<T> 模板，封装 C++ 设备对象，管理 GIL 释放 |
| `controller.hpp` | IControllerWrapper，实时控制器 Python 绑定 |
| `filter.hpp` | LowPass 滤波器 Python 绑定 |
| `logging.hpp` | 日志配置函数 Python 绑定 |

**核心设计模式 (wrapper.hpp)**:

```cpp
template <typename T>
class Wrapper : private T {
    // 模板方法
    read<Data>(timeout)              // 同步读
    read_async<Data>(timeout)        // 异步读
    read_async_unchecked(timeout)    // 非检查读
    write<Data>(value, timeout)      // 同步写
    write_async<Data>(value, timeout)// 异步写
    write_async_unchecked(...)       // 非检查写
    get<Data>()                      // 缓存获取
};
```

---

### 3. C++ SDK 层 (wujihandcpp/)

#### 3.1 设备层 (device/)

**职责**: 设备抽象和操作

| 文件 | 职责 |
|------|------|
| `hand.hpp` | Hand 类，USB 设备管理，批量操作 |
| `finger.hpp` | Finger 类，单手指管理 |
| `joint.hpp` | Joint 类，单关节管理 |
| `controller.hpp` | IController 接口，实时控制器 |
| `data_operator.hpp` | DataOperator<T> 模板，统一的读写接口基类 |
| `latch.hpp` | Latch 同步原语，批量操作等待 |
| `data_tuple.hpp` | 数据元组管理 |
| `helper.hpp` | 设备辅助函数 |

**设备层次模型**:
```
Hand (灵巧手)
├── finger[0]  # 拇指
│   └── joint[0-3]  # MCP, DIP, PIP, IP
├── finger[1]  # 食指
│   └── joint[0-3]
├── finger[2]  # 中指
│   └── joint[0-3]
├── finger[3]  # 无名指
│   └── joint[0-3]
└── finger[4]  # 小指
    └── joint[0-3]
```

#### 3.2 数据层 (data/)

**职责**: 定义 CANopen 数据类型和元数据

| 文件 | 职责 |
|------|------|
| `hand.hpp` | Hand 只读数据: Handedness, FirmwareVersion, Temperature 等 |
| `joint.hpp` | Joint 数据: ControlMode, EffortLimit, Position 等 |

**数据类型模板**:

```cpp
// 只读数据
struct FirmwareVersion : ReadOnlyData<device::Joint, 0x01, 1, uint32_t> {};

// 读写数据
struct EffortLimit : ReadWriteData<device::Joint, 0x07, 2, double> {};

// 只写数据
struct TargetPosition : WriteOnlyData<device::Joint, 0x7A, 0, double> {};
```

#### 3.3 协议层 (protocol/)

**职责**: CANopen 协议处理

| 文件 | 职责 |
|------|------|
| `handler.hpp` | Protocol::Handler，协议处理器主类 |
| `frame_builder.hpp` | 帧构建器，CANopen 报文组装 |
| `protocol.hpp` | 协议常量定义 |
| `latency_tester.hpp` | USB 延迟测试 |

**通信模式**:
- **SDO (Service Data Objects)**: 请求/响应模式，用于配置操作，默认超时 500ms
- **PDO (Process Data Objects)**: 高速循环数据，实时运动控制可达 1kHz

#### 3.4 传输层 (transport/)

**职责**: USB 设备通信

| 文件 | 职责 |
|------|------|
| `usb.cpp` | libusb-1.0 USB 传输实现 |

**USB 设备信息**:
- VID: `0x0483`
- PID: `0x7530`

#### 3.5 滤波器层 (filter/)

**职责**: 信号滤波

| 文件 | 职责 |
|------|------|
| `low_pass.hpp` | 低通滤波器，用于平滑控制信号 |

#### 3.6 工具层 (utility/)

**职责**: 通用工具

| 文件 | 职责 |
|------|------|
| `logging.hpp` | spdlog 日志系统 |
| `api.hpp` | API 常量定义 |
| `ring_buffer.hpp` | 环形缓冲区 |
| `singleton.hpp` | 单例模式模板 |

---

## 依赖关系

### 外部依赖

| 依赖 | 版本 | 用途 |
|------|------|------|
| Python | 3.8+ | 运行时 |
| pybind11 | - | Python/C++ 绑定 |
| numpy | - | 数组操作 |
| libusb | 1.0+ | USB 通信 |
| spdlog | - | 日志 |
| scikit-build-core | - | 构建系统 |

### 内部依赖

```
src/wujihandpy/__init__.py
    └── src/_core (编译后的 C++ 扩展)
            └── src/*.hpp (绑定层)
                    └── wujihandcpp/ (C++ SDK)
                            ├── device/ → data/, protocol/, utility/
                            ├── protocol/ → transport/, utility/
                            └── utility/ (独立)
```

---

## 构建配置

### pyproject.toml

```toml
[build-system]
requires = ["scikit-build-core", "pybind11"]

[project]
name = "wujihandpy"
dependencies = ["numpy"]

[tool.cibuildwheel]
archs = ["native"]  # x86_64, ARM64
skip = "*-musllinux*"
```

### CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.15)
project(wujihandpy)
set(CMAKE_CXX_STANDARD 20)

add_subdirectory(wujihandcpp EXCLUDE_FROM_ALL)
pybind11_add_module(_core src/*.cpp)
target_link_libraries(_core PRIVATE wujihandcpp)
```

---

## 示例文件

| 文件 | 内容 |
|------|------|
| `example/1.read.py` | 同步读取操作 |
| `example/2.write.py` | 同步写入操作 |
| `example/3.realtime.py` | 实时控制器使用 |
| `example/4.async.py` | 异步操作 |

---

## 设计模式

### 1. DataOperator<T> 模板

统一的读写接口基类，封装 CANopen 地址信息：

```cpp
template <typename Data>
class DataOperator {
    read(timeout)           // 同步读取
    read_async(callback)    // 异步读取
    write(value, timeout)   // 同步写入
    write_async(...)        // 异步写入
};
```

### 2. Wrapper<T> 模板

Python 绑定层的核心模板，管理 GIL 和生命周期：

- 热路径使用 `py::gil_scoped_release` 释放 GIL
- 批量操作性能优化
- 异步操作支持 (asyncio)

### 3. Latch 同步原语

C++20 原子操作实现的批量异步同步：

```cpp
wujihandcpp::device::Latch latch;
// 并发写入多个关节
for (int i = 0; i < 5; i++)
    for (int j = 0; j < 4; j++)
        hand.finger(i).joint(j).write_async<Data>(latch, value, timeout);
latch.wait();  // 等待所有操作完成
```

### 4. Pimpl 模式

Handler::Impl 隐藏实现细节，保证 ABI 稳定性：

```cpp
class Handler {
public:
    // 公开接口
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
```

---

## 编译与安装

### 开发环境

```bash
# 创建虚拟环境
python3 -m venv .venv
source .venv/bin/activate

# 安装开发依赖
pip install scikit-build-core pybind11 numpy

# 构建
cmake --preset linux
cmake --build build -j$(nproc)

# 安装
pip install -e .
```

### 发布构建

```bash
python -m cibuildwheel --output-dir wheelhouse
```

---

## 代码规范

| 规范 | 说明 |
|------|------|
| C++ 标准 | C++20 (GCC 13+ 或 Clang 17+) |
| Python | 3.8+ |
| 格式化 | clang-format (LLVM 风格, 100 列, 4 空格) |
| 静态分析 | clang-tidy |

---

## 注意事项

1. **USB 权限**: Linux 需要配置 udev 规则
2. **虚拟环境**: 始终在虚拟环境中开发
3. **类型存根**: 修改 C++ 绑定后运行 `python update_stubs.py`
4. **异常转换**: C++ `TimeoutError` 转换为 Python `TimeoutError`
5. **J1 反转**: J1 关节（拇指除外）位置值会被自动反转
