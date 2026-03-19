# Wuji Hand Zenoh Bridge

将 WujiHand 灵巧手通过 Zenoh 协议暴露到网络中，使 wuji-sdk 客户端可以透明地发现和控制灵巧手。

## 架构

```text
wuji-sdk 客户端 ←→ Zenoh 网络 ←→ Hand Bridge ←→ USB ←→ 灵巧手硬件
```

Bridge 持有灵巧手的独占 USB 连接，向 Zenoh 网络注册 Queryable（GET/SET）和 Publisher（SUB 订阅流）。wuji-sdk 客户端通过 Zenoh liveliness 发现设备，通过 `@capability` 查询资源列表，然后透明交互——与访问手套等其他设备完全一致。

## 两个版本

| | Python | C++ |
|---|---|---|
| 路径 | `bridge/python/` | `bridge/cpp/` |
| 依赖 | eclipse-zenoh, wujihandpy, numpy | zenoh-cpp, wujihandcpp, nlohmann/json |
| 适用场景 | 开发调试、快速迭代 | 生产部署、低延迟 |
| 控制路径 | 100Hz 喂数线程 → controller | Zenoh 回调直接写 controller |

## 快速开始

### Python 版

```bash
cd wujihandpy
source .venv/bin/activate
pip install eclipse-zenoh numpy

# 启动（手必须 USB 连接，--pub-rate 必填）
PYTHONPATH=. python -m bridge.python.hand_zenoh_bridge --pub-rate 1000

# 完整参数
PYTHONPATH=. python -m bridge.python.hand_zenoh_bridge \
    --sn "DEVICE_SN" \
    --pub-rate 1000 \
    --log-level DEBUG
```

### C++ 版

```bash
cd bridge/cpp
mkdir -p build && cd build
cmake .. && cmake --build . -j$(nproc)

# 启动（--pub-rate 必填）
./wujihand_zenoh_bridge --pub-rate 1000 --log-level info

# 完整参数
./wujihand_zenoh_bridge --sn "DEVICE_SN" --pub-rate 1000 --log-level debug
```

## 客户端使用

```python
import zenoh, json

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
    # 1. 发现设备（通过 liveliness）
    replies = session.liveliness().get("wuji/**")

    # 2. 查询能力
    replies = session.get(f"wuji/{sn}/@capability", timeout=5.0)

    # 3. 获取控制权（先声明 control-owner liveliness token）
    session.get(f"wuji/{sn}/@control", payload=f"acquire:{zid}".encode(), timeout=5.0)

    # 4. 读取数据（GET）
    replies = session.get(f"wuji/{sn}/joint/actual_position", timeout=5.0)

    # 5. 写入目标位置（低延迟 fire-and-forget）
    session.put(f"wuji/{sn}/joint/target_position", json.dumps(target).encode())

    # 6. 订阅实时数据流（频率由 --pub-rate 配置）
    sub = session.declare_subscriber(f"wuji/{sn}/joint/actual_position", callback)
finally:
    session.get(f"wuji/{sn}/@control", payload=f"release:{zid}".encode(), timeout=5.0)
    owner_token.undeclare()
    if sub is not None:
        sub.undeclare()
    session.close()
```

## 资源列表

### GET 资源（只读）

| 路径 | 类型 | 说明 |
|------|------|------|
| `input_voltage` | number | 输入电压 |
| `temperature` | number | 设备温度 |
| `handedness` | integer | 左/右手 |
| `firmware_version` | integer | 固件版本号 |
| `joint/actual_position` | 5×4 float | 关节实际位置（SUB 频率由 `--pub-rate` 配置） |
| `joint/actual_effort` | 5×4 float | 关节实际力矩（SUB 频率由 `--pub-rate` 配置） |
| `joint/temperature` | 5×4 float | 关节温度 |
| `joint/error_code` | 5×4 int | 关节错误码 |
| `joint/effort_limit` | 5×4 float | 力矩限制（可读可写）|
| `joint/upper_limit` | 5×4 float | 关节上限 |
| `joint/lower_limit` | 5×4 float | 关节下限 |
| `joint/bus_voltage` | 5×4 float | 关节总线电压 |

### SET 资源（需要控制权）

| 路径 | 类型 | 说明 |
|------|------|------|
| `joint/target_position` | 5×4 float | 目标位置 |
| `joint/control_mode` | 5×4 int | 控制模式 |
| `joint/enabled` | 5×4 bool | 关节使能 |
| `joint/effort_limit` | 5×4 float | 力矩限制 |
| `joint/reset_error` | 5×4 int | 错误复位 |

### SUB 数据流格式

SUB 资源以时间戳信封格式推送：

```json
{
  "timestamp_us": 1773822692412074,
  "data": [[0.1, 0.2, 0.3, 0.4], ...]
}
```

## 控制权协议

- **获取**: 发送 `acquire:{your_zid}` → 返回 `granted` 或 `denied:{current_owner}`
- **释放**: 发送 `release:{your_zid}` → 返回 `released`
- **查询**: 空 payload → 返回当前 owner 或 `none`
- **崩溃自动释放**: Bridge 通过 Zenoh liveliness 监控控制方存活状态，进程崩溃时自动释放

## 测试

```bash
# 单元测试（无需硬件）
cd wujihandpy
source .venv/bin/activate
PYTHONPATH=. python -m pytest tests/test_bridge.py -v

# 正弦波硬件测试（需要启动 bridge + 连接手）
python example/6.zenoh_realtime.py --duration 10
```
