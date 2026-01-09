# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

WujihandPy 是 WujiHandCpp SDK（灵巧手机械手）的 Python 绑定库。通过 pybind11 提供 Python 接口，支持 USB 通信控制灵巧手设备。

- 支持 Python 3.8-3.14
- 仅支持 Linux（x86_64, ARM64）
- 发布至 PyPI: `wujihandpy`

## Build Commands

```bash
# 创建并激活虚拟环境（首次）
python3 -m venv .venv
source .venv/bin/activate

# 安装开发依赖
pip install scikit-build-core pybind11 numpy

# 快速开发迭代（推荐）
# 每次修改 C++ 代码后执行以下命令完成编译和安装
cmake --preset linux && cmake --build build -j$(nproc) && \
cp build/_core.cpython-*-linux-gnu.so src/wujihandpy/ && \
pip install -e .

# 强制完整重编译（当增量编译未检测到变更时使用）
rm -rf build && cmake --preset linux && cmake --build build -j$(nproc) && \
cp build/_core.cpython-*-linux-gnu.so src/wujihandpy/ && \
pip install -e .

# 构建发布包
python -m cibuildwheel --output-dir wheelhouse

# 更新 Python 类型存根（修改 C++ 绑定后必须执行）
python update_stubs.py

# 格式化 C++ 代码
clang-format -i src/*.cpp src/*.hpp

# 静态分析
clang-tidy -p build src/main.cpp
```

**注意**: 始终在虚拟环境中开发，避免污染系统 Python 环境。

## Architecture

**三层架构：**

1. **Python 层** (`src/wujihandpy/`) - Pythonic API，含类型存根（`.pyi`）
2. **pybind11 绑定层** (`src/`) - C++ 与 Python 的桥接
3. **C++ SDK 层** (`wujihandcpp/`) - 核心设备通信实现

**设备层次模型：**
```
Hand (灵巧手)
  └── Finger[5] (5个手指)
      └── Joint[4] (每个手指4个关节)
```

**通信协议：**
- **SDO (Service Data Objects)**: 请求/响应模式，用于配置，默认超时500ms
- **PDO (Process Data Objects)**: 高速循环数据，实时运动控制可达1kHz

### Python 绑定层 (`src/`)

| 文件 | 用途 |
|------|------|
| `main.cpp` | pybind11 模块入口，异常转换 |
| `wrapper.hpp` | 核心 Wrapper<T> 模板，封装 Hand/Finger/Joint，管理 GIL |
| `controller.hpp` | 实时控制器绑定 |
| `filter.hpp` | 滤波器绑定 |
| `logging.hpp` | 日志 API 绑定 |

**API 模式（每个数据字段）：**
- `read_<name>()` / `read_<name>_async()` / `read_<name>_unchecked()`
- `write_<name>(value)` / `write_<name>_async(value)` / `write_<name>_unchecked(value)`
- `get_<name>()`: 非阻塞缓存获取

### C++ SDK 层 (`wujihandcpp/`)

**目录结构：**
```
wujihandcpp/
├── include/wujihandcpp/    # 公开 API 头文件
│   ├── device/             # Hand, Finger, Joint, Controller
│   ├── data/               # 数据类型定义（编译期元数据）
│   ├── filter/             # 低通滤波器
│   ├── protocol/           # 协议处理
│   └── utility/            # 日志和工具
├── src/                    # 实现文件
│   ├── device/             # Latch 同步原语
│   ├── protocol/           # 帧构建器、延迟测试
│   ├── transport/          # USB 传输层 (libusb-1.0)
│   └── logging/            # spdlog 日志
└── example/                # C++ 使用示例
```

**核心设计模式：**
- **DataOperator<T> 模板**: 统一的读写接口基类
- **编译期数据元数据**: `ReadOnlyData<Base, index, sub_index, ValueType>` 携带 CANopen 地址信息
- **Latch 同步**: C++20 原子操作实现的批量异步同步原语
- **Pimpl 模式**: Handler::Impl 隐藏实现细节保证 ABI 稳定性

**关节数据类型：**
- 只读: FirmwareVersion, Temperature, ErrorCode, ActualPosition, UpperLimit, LowerLimit
- 只写: ControlMode, CurrentLimit, Enabled, TargetPosition

## Code Style

- **C++ 标准**: C++20（需 GCC 13+ 或 Clang 17+）
- **格式化**: clang-format (LLVM 风格，100 列限制，4 空格缩进)
- **静态分析**: clang-tidy（配置于 `.clang-tidy`）

## Development Notes

- **构建后必须运行 `pip install -e .`**：editable install 会将 `src/wujihandpy/` 下的 .so 文件复制到 site-packages，Python 实际加载的是 site-packages 中的文件
- 修改 C++ 绑定后必须运行 `python update_stubs.py` 同步类型存根
- C++ 异常 `TimeoutError` 会转换为 Python `TimeoutError`
- 热路径使用 `py::gil_scoped_release` 释放 GIL 提升性能
- 批量操作性能远优于单独调用（建议使用 Hand 级别 API）
- J1 关节（拇指除外）位置值会被反转
- USB 设备: VID=0x0483, PID=0x7530
