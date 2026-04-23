# wujihandpy 架构设计

> 灵巧手 Python SDK 架构概览，覆盖 C++ 核心 → Python 绑定 → Zenoh Bridge 三层。

## 1. 整体数据流

```
  ┌──────────────────────────────────────────────────────────────────────┐
  │                          硬件设备                                    │
  │                                                                      │
  │   ┌─────────────┐                    ┌─────────────┐                │
  │   │ Wuji Hand   │                    │ TouchBoard  │                │
  │   │ (CANopen)   │                    │ (USB CDC)   │                │
  │   └──────┬──────┘                    └──────┬──────┘                │
  └──────────┼───────────────────────────────────┼──────────────────────┘
             │                                   │
  ═══════════╪═══════════════════════════════════╪════════════════════════
             │          C++ 核心层               │
             ▼                                   ▼
  ┌─────────────────┐                 ┌───────────────────┐
  │ Handler         │                 │ TactileParser     │
  │ (SDO/PDO 协议)  │                 │ (帧同步+CRC校验)  │
  └────────┬────────┘                 └────────┬──────────┘
           │                                   │
           ▼                                   ▼
  ┌─────────────────┐                 ┌───────────────────┐
  │ Hand            │                 │ TouchBoard::Impl  │
  │ 5×Finger×4Joint │                 │ 帧缓存+归一化+FPS │
  └────────┬────────┘                 └────────┬──────────┘
           │                                   │
  ═════════╪═══════════════════════════════════╪══════════════════════════
           │        pybind11 绑定层            │
           ▼                                   ▼
  ┌─────────────────┐                 ┌───────────────────┐
  │ Wrapper<Hand>   │                 │ TouchBoardWrapper │
  │ GIL释放+NumPy   │                 │ 固定数组→NumPy    │
  └────────┬────────┘                 └────────┬──────────┘
           │                                   │
  ═════════╪═══════════════════════════════════╪══════════════════════════
           │       Python Bridge 层            │
           ▼                                   ▼
  ┌─────────────────┐                 ┌───────────────────┐
  │ HandBridge      │                 │ TactileBridge     │
  │ 资源模型+控制权  │                 │ 触觉数据发布       │
  └────────┬────────┘                 └────────┬──────────┘
           │                                   │
           └──────────────┬────────────────────┘
                          ▼
                 ┌─────────────────┐
                 │  Zenoh Network  │
                 └────────┬────────┘
                          ▼
                 ┌─────────────────┐
                 │ wuji-sdk 消费端  │
                 └─────────────────┘
```

## 2. 目录结构

```
wujihandpy/
├── wujihandcpp/                        # C++ 核心库
│   ├── include/wujihandcpp/
│   │   ├── device/                     # Hand, Finger, Joint, TouchBoard
│   │   ├── data/                       # 编译期数据类型定义
│   │   └── protocol/                   # 协议接口
│   └── src/
│       ├── transport/                  # USB 传输层 (libusb)
│       ├── protocol/                   # Handler, TactileParser
│       └── device/                     # TouchBoard 实现
│
├── src/
│   ├── main.cpp                        # pybind11 模块入口
│   ├── wrapper.hpp                     # Hand/Finger/Joint 通用绑定
│   ├── touch_board_wrapper.hpp         # TouchBoard 绑定
│   └── wujihandpy/
│       ├── __init__.py                 # 对外导出
│       └── bridge/                     # Zenoh Bridge（可选依赖）
│           ├── hand_zenoh_bridge.py
│           ├── tactile_bridge.py
│           └── cli.py
│
├── examples/                           # 集成演示
├── tests/                              # Python 单测
└── pyproject.toml                      # 包配置 (bridge 作为 optional extra)
```

核心模块职责：

| 模块 | 职责 |
| --- | --- |
| `transport/usb.cpp` | libusb 封装，设备发现，bulk 传输 |
| `protocol/handler.cpp` | CANopen SDO/PDO 协议处理 |
| `protocol/tactile_parser.hpp` | USB 字节流 → 触觉帧的流式状态机 |
| `device/touch_board.cpp` | 帧缓存、归一化、FPS、阻塞读 |
| `wrapper.hpp` | C++ Hand/Finger/Joint → Python 方法自动生成 |
| `touch_board_wrapper.hpp` | C++ 固定数组 → NumPy 数组转换 |
| `hand_zenoh_bridge.py` | Hand → Zenoh 资源模型 + 控制权协议 |
| `tactile_bridge.py` | TouchBoard → Zenoh 触觉数据发布 |

## 3. C++ 核心层

### 3.1 USB 设备发现

传输层通过 `ITransport` 接口抽象底层通信。设备发现流程：

```
  开始: libusb_get_device_list
    │
    ▼
  ┌──────────────────────┐
  │ 遍历每个 USB 设备     │
  └──────────┬───────────┘
             ▼
       VID 匹配？ ──否──→ 跳过
             │是
             ▼
       有序列号？ ──否──→ 跳过
             │是
             ▼
    指定了 PID？ ──否──→ 加入候选
             │是          │
             ▼            │
       PID 匹配？ ─否─→ 跳过
             │是          │
             ▼            │
    指定了 SN？ ──否──→ 加入候选
             │是          │
             ▼            │
       SN 匹配？ ──否──→ 跳过
             │是          │
             ▼            │
          加入候选 ←───────┘
             │
             ▼
     ┌───────────────┐
     │ 候选数 == 1？  │
     └───┬───────┬───┘
        是      否
         │       │
         ▼       ▼
      选中设备  报错+诊断
```

规则：VID → 可选 PID → 可选 SN 逐级过滤，最终必须恰好 **1 台设备**。

### 3.2 TactileParser 状态机

TouchBoard 通过 USB CDC 传输触觉帧。`TactileParser` 是流式解析器，不假设 USB 回调按帧对齐：

```
           ┌──────────────────────────────────────────────┐
           │                                              │
           ▼                                              │
  ┌─────────────────┐                                     │
  │    SYNC_AA      │ ←─── 其他字节（循环等待）            │
  │  等待 0xAA      │                                     │
  └────────┬────────┘                                     │
           │ 收到 0xAA                                    │
           ▼                                              │
  ┌─────────────────┐                                     │
  │    SYNC_55      │ ←─── 再收到 0xAA（保持此状态）       │
  │  等待 0x55      │                                     │
  └────────┬────────┘                                     │
           │ 收到 0x55                                    │
           ▼                                              │
  ┌─────────────────┐                                     │
  │   ACCUMULATE    │ ←─── 持续收字节                      │
  │  收满 1550 字节  │                                     │
  └────────┬────────┘                                     │
           │ 收满                                         │
           ▼                                              │
     CRC16 校验 ──通过──→ 输出帧 ──→ 回到 SYNC_AA ────────┘
           │
         失败
           │
           ▼
     扫描 buffer 中残余 0xAA55
           │
     ┌─────┴─────┐
   找到          未找到
     │             │
     ▼             ▼
  从该位置       回到 SYNC_AA
  继续解析
```

帧格式（1550 字节）：

| 偏移 | 长度 | 内容 |
|------|------|------|
| 0 | 2 | 同步头 `0xAA 0x55` |
| 2 | 2 | 帧长度 u16 LE |
| 4 | 1 | handedness（0=左, 1=右）|
| 6 | 1536 | 触觉数据 24×32 × int16 LE |
| 1542 | 2 | 序列号 u16 LE |
| 1544 | 4 | 设备时间戳 ms u32 LE |
| 1548 | 2 | CRC16-CCITT |

### 3.3 TouchBoard 帧处理

```
  USB 回调收到字节块
    │
    ▼
  parser.feed(data, size)
    │
    ▼
  解析出 N 帧？ ──否──→ 等待更多字节
    │是
    ▼
  取最后一帧（latest-frame 语义）
    │
    ├──→ memcpy raw_data[24][32]
    │
    ├──→ 归一化: normalized = 1.0 - raw / 2135.0（ADC 开路值）
    │
    ├──→ 更新 handedness / sequence / timestamp
    │
    ├──→ frame_count += N
    │
    ├──→ 更新 FPS（滑动窗口 1 秒）
    │
    └──→ cv.notify_all（唤醒阻塞读取者）
```

读取模式：

| 模式 | 函数 | 语义 |
|------|------|------|
| 非阻塞 | `get_tactile()` | 返回最新缓存，无帧返回 None |
| 阻塞 | `read_tactile(timeout)` | 等待 frame_count 递增（保证读到新帧） |

### 3.4 Hand 设备层级

Hand 采用三层模型：`Hand → 5×Finger → 4×Joint`（共 20 个关节）。

通信协议：
- **SDO**：同步读写单个数据对象，5~10ms 延迟，用于配置
- **PDO**：周期性数据交换，2ms 间隔，用于实时控制

实时控制器根据固件版本自动选择策略：

```
  hand.realtime_controller(filter)
    │
    ▼
  固件支持 16kHz 滤波器？
    │
  ┌─┴──────────────────────┐
  是                        否
  │                         │
  ▼                         ▼
  CompatibleController     FilteredController
  固件侧做滤波              主机侧做滤波
  主机只发目标值             通过 PDO 2kHz 发送
```

## 4. Python 绑定层

### 4.1 绑定策略

两类设备使用不同的绑定策略：

| 设备 | 绑定方式 | 原因 |
|------|---------|------|
| Hand/Finger/Joint | `Wrapper<T>` 模板 | 统一 DataOperator 读写接口 |
| TouchBoard | 独立 `TouchBoardWrapper` | 不走 CANopen，协议完全不同 |

关键机制：
- 所有阻塞操作（read/write/read_tactile）进入 C++ 前释放 GIL
- C++ 固定数组通过 `new` + `py::capsule` 转为独立 NumPy 数组
- 超时统一抛出 Python `TimeoutError`

### 4.2 Bridge 模块

Bridge 作为可选依赖（`pip install wujihandpy[bridge]`），将本地设备发布为 Zenoh 网络资源。

**HandBridge 资源模型**（17 个资源）：

| Zenoh Key | 类型 | 说明 |
|-----------|------|------|
| `wuji/{sn}/@alive` | Liveliness | 在线探测 |
| `wuji/{sn}/@capability` | GET | 能力描述 |
| `wuji/{sn}/@control` | GET | 控制权获取/释放 |
| `wuji/{sn}/joint/actual_position` | SUB | 关节位置（周期发布） |
| `wuji/{sn}/joint/target_position` | PUT Sub | 目标位置写入 |
| ... | | 共 12 GET + 5 SET |

**HandBridge 启动顺序**：

```
  start()
    │
    ├─ 1. 打开 Zenoh session
    │
    ├─ 2. 声明 @alive liveliness token
    │
    ├─ 3. 配置控制模式 → 使能关节 → 启动 realtime controller
    │     └─ 启动 _realtime_loop (100Hz 喂目标值)
    │
    ├─ 4. put @status = online  ← 在控制器就绪后才上线
    │
    ├─ 5. 声明 @capability / @control queryable
    │
    ├─ 6. 声明资源 queryable（GET/SET）
    │
    ├─ 7. 订阅 target_position（低延迟 PUT 路径）
    │
    ├─ 8. 启动 _publish_loop（SUB 资源周期发布）
    │
    └─ 完成: "Bridge fully started"
```

关键：先把 realtime controller 准备好，再公开 online 状态。

**控制权协议**：

```
  控制端                    HandBridge               Zenoh Liveliness
    │                          │                          │
    │ @control acquire:zid     │                          │
    │─────────────────────────→│                          │
    │                          │ 校验 attachment == zid   │
    │                          │                          │
    │                    ┌─────┴─────┐                    │
    │                  无 owner    有其他 owner            │
    │                    │           │                     │
    │                    ▼           ▼                     │
    │              设置 owner    denied:current_owner      │
    │                    │                                 │
    │                    ▼                                 │
    │              订阅 owner liveliness                   │
    │                    │                                 │
    │←── granted ────────┘                                 │
    │                                                      │
    │        ... owner 进程崩溃 ...                         │
    │                          │                           │
    │                          │←── SampleKind.DELETE ─────│
    │                          │                           │
    │                          ▼                           │
    │                   _control_owner = None               │
    │                   (自动回收控制权)                     │
```

**target_position 低延迟路径**：

```
  Zenoh PUT joint/target_position
    │
    ▼
  _handle_target_position_put
    │
    ├─ requester 是当前 owner？ ──否──→ 忽略+告警
    │是
    ├─ json.loads payload
    │
    ├─ 校验 shape == (5,4) 且值有限
    │
    ├─ 原子更新 _rt_target    ← 非阻塞，不走 SDO
    │
    │    ┌──────────────────────────────────────────┐
    │    │ _realtime_loop (独立线程, 100Hz)          │
    │    │                                          │
    │    │   读取 _rt_target                         │
    │    │     │                                    │
    │    │     ▼                                    │
    │    │   controller.set_joint_target_position   │
    │    │     │                                    │
    │    │     ▼                                    │
    │    │   PDO 1kHz 下发到关节                     │
    │    └──────────────────────────────────────────┘
    │
    └─ 网络命令层与实时控制层解耦
```

**TactileBridge** 结构更简单，只做单向数据发布：

| Zenoh Key | 说明 |
|-----------|------|
| `wuji/tboard_{sn}/@alive` | 在线探测 |
| `wuji/tboard_{sn}/@capability` | 能力描述 (rows, cols, handedness) |
| `wuji/tboard_{sn}/tactile` | 归一化 float32 (pub_rate Hz) |
| `wuji/tboard_{sn}/tactile_raw` | 原始 int16 ADC |

发布循环从同一份 raw 数据计算 tactile 和 tactile_raw，共享同一个 host timestamp，避免时序漂移。

## 5. 关键设计决策

| 决策 | 原因 |
|------|------|
| TouchBoard 只保留 latest frame | 偏向实时观测，降低内存开销 |
| 阻塞读等待 frame_count 递增 | 保证读到新帧而非旧缓存 |
| target_position 只更新原子缓存 | 网络命令与实时控制解耦，避免 SDO 延迟抖动 |
| Bridge 作为 optional extra | 核心包不依赖 Zenoh，按需安装 |
| SUB 数据统一包 host timestamp | 跨系统消费友好，统一时间基准 |
| 触觉数据 flatten 为一维数组 | 减小 JSON 嵌套，矩阵形状靠 capability 补充 |
| 控制权通过 liveliness TTL 回收 | 防止 owner 崩溃后控制权泄漏 |
| 固件版本协商驱动特性开关 | 同一代码兼容不同版本固件 |
| TouchBoard 不复用 Wrapper/DataOperator | 协议完全不同，硬套模板是过度抽象 |
| 两个 Bridge 不抽公共基类 | 复杂度差异太大，强行统一增加不必要的间接层 |
