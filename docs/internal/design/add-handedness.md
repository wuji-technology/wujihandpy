# Hand 构造支持按左右手识别（side 参数）

| 版本 | 日期 | 作者 | 变更说明 |
|------|------|------|----------|
| v1.0 | 2026-05-19 | 彭绍辉 | 初版 |

关联工作项：[m-6993430758](https://project.feishu.cn/uts5wn/story/detail/6993430758)

## 背景

当前 SDK 在主机插了多只灵巧手时只能用 `serial_number` 选设备：

```python
left = Hand(serial_number='WH123...')
right = Hand(serial_number='WH456...')
```

实际使用中大多数场景就是"左+右"两只手，记录 SN 对用户既麻烦又脆弱（换手要改代码）。issue 提的诉求是直接用 handedness 选：

```python
left = Hand(side='left')
right = Hand(side='right')
```

固件侧已经在 SDO `0x5090:0` 暴露 handedness 字段（`data::hand::Handedness`，1 字节 uint8），值约定 **`0 = Right, 1 = Left`**（来源：`docs/external/en/api-reference.mdx:228` 的 `read_handedness ... (0=right, 1=left)`）。注意这个约定与 `wujihandcpp/include/wujihandcpp/data/tactile.hpp` 里触觉模块的 `enum class Handedness { LEFT = 0, RIGHT = 1 }` **相反**，灵巧手以 api-reference 为准。

## 关键约束

handedness 是**SDO 字段，不在 USB 描述符里**，意味着无法在 libusb 枚举阶段直接过滤。必须先 USB 连接到候选设备、跑一次 SDO read 才能拿到 handedness。

## 方案

### 总体思路

新增 `Hand(side='left' | 'right')` 构造入口，内部"枚举 USB → 逐个轻量 probe SDO → 找到匹配 side 的设备 SN → 用原有 SN 路径正式构造"。SN 路径完全不变，纯增量。

### API 形态

- `side` 和 `serial_number` **互斥**，同传抛 `ValueError`
- 探测层放在 **C++ 层**（`wujihandcpp`），保持 Python 侧仅做参数透传
- 错误用现有 `ConnectionError` + 详细 message，**不新增异常类**（与现有 `TimeoutError` / `ConnectionError` 二分法保持一致）

### 探测路径

利用现成的 `protocol::Handler::raw_sdo_read` —— **不需要给 Handler 加新模式**，因为：

- Probe 不需要 `init_storage_info`（raw_sdo_read 不依赖 storage 注册）
- Probe 不需要走 `Hand::check_firmware_version` 重路径（那个会跑 20+ 条 SDO）
- 一次 probe 只做：`Handler` 构造（含 USB enumerate + claim）→ `start_transmit_receive` → 一次 `raw_sdo_read(0x5090, 0, 200ms)` → 析构（RAII 自动 libusb_close）

单只 probe 开销估计 < 100ms。

### C++ 层改动

**1. 新增 `wujihandcpp::transport` 枚举辅助函数**

`transport/usb.cpp` 的 `select_device` 内部已经做了 VID/PID 枚举 + libusb_open + iSerialNumber 读取的全部工作，但当前只暴露"选 1 个"的语义。把"列出所有匹配 VID/PID 设备的 SN"抽出为公开静态函数：

```cpp
// 新增到 wujihandcpp/include/wujihandcpp/transport/usb_enumerate.hpp（或类似位置）
namespace wujihandcpp::transport {
std::vector<std::string> list_matching_serial_numbers(uint16_t vid, int32_t pid);
}
```

实现复用 `select_device` 里枚举 + descriptor 读取逻辑，但不做 claim_interface、不持有 handle。建议把枚举部分抽成一个内部公共函数，让 `select_device` 也复用，避免两份相似的 USB 描述符遍历代码漂移。

**2. `Hand` 新增 Side enum 和按 side 构造**

```cpp
// wujihandcpp/include/wujihandcpp/device/hand.hpp
class Hand : public DataOperator<Hand> {
public:
    // 值与固件约定一致：0 = Right, 1 = Left（见 api-reference.mdx）
    enum class Side : uint8_t { Right = 0, Left = 1 };

    // 现有构造，保持不变
    explicit Hand(const char* serial_number = nullptr,
                  int32_t usb_pid = 0x2000, uint16_t usb_vid = 0x0483,
                  uint32_t mask = 0);

    // 新增：按 handedness 探测后用 SN 路径构造
    explicit Hand(Side side,
                  int32_t usb_pid = 0x2000, uint16_t usb_vid = 0x0483,
                  uint32_t mask = 0)
        : Hand(probe_handedness(side, usb_vid, usb_pid).c_str(),
               usb_pid, usb_vid, mask) {}

private:
    static std::string probe_handedness(Side side, uint16_t vid, int32_t pid);
};
```

`probe_handedness` 伪代码：

```cpp
std::string Hand::probe_handedness(Side side, uint16_t vid, int32_t pid) {
    auto serials = transport::list_matching_serial_numbers(vid, pid);
    if (serials.empty())
        throw ConnectionError(
            "No device found for VID=0x" + hex(vid) + " PID=0x" + hex(pid));

    std::vector<std::string> matches;
    std::vector<std::pair<std::string, std::string>> probe_failures;  // (sn, reason)

    for (const auto& sn : serials) {
        try {
            protocol::Handler probe(vid, pid, sn.c_str(), /*storage_unit_count=*/0);
            probe.start_transmit_receive();
            auto bytes = probe.raw_sdo_read(0x5090, 0, 200ms);
            if (!bytes.empty() && static_cast<Side>(bytes[0]) == side)
                matches.push_back(sn);
        } catch (const TimeoutError& e) {
            probe_failures.push_back({sn, std::string("timeout: ") + e.what()});
        } catch (const ConnectionError& e) {
            probe_failures.push_back({sn, std::string("connect: ") + e.what()});
        }
    }

    if (matches.size() == 1)
        return matches[0];

    // 失败诊断：列出所有 SN 及结果（matched / not matched / probe failed）
    // 注：以下 message 里的 <side> 实现时替换为字面值 "left" / "right"
    std::string msg = matches.empty()
        ? "No <side> hand found"
        : "Multiple <side> hands found";
    msg += "; saw " + std::to_string(serials.size()) + " device(s):";
    for (auto& sn : serials) msg += " " + sn;
    if (!probe_failures.empty()) {
        msg += "; probe failures:";
        for (auto& [sn, reason] : probe_failures)
            msg += " " + sn + "(" + reason + ")";
    }
    msg += matches.empty()
        ? "; if firmware does not expose handedness, use serial_number"
        : "; use serial_number to disambiguate";
    throw ConnectionError(msg);
}
```

**3. pybind11 绑定（`src/main.cpp`）**

注册 Side enum 并加 init overload：

```cpp
py::enum_<wujihandcpp::device::Hand::Side>(hand, "Side")
    .value("Left", wujihandcpp::device::Hand::Side::Left)
    .value("Right", wujihandcpp::device::Hand::Side::Right);

hand.def(
    py::init<wujihandcpp::device::Hand::Side, int32_t, uint16_t,
             std::optional<py::array_t<bool>>>(),
    py::arg("side"), py::arg("usb_pid") = 0x2000,
    py::arg("usb_vid") = 0x0483, py::arg("mask") = py::none());
```

### Python 层改动

`src/wujihandpy/__init__.py`：

```python
class Hand(_core.Hand):
    def __init__(self, *, side=None, serial_number=None,
                 usb_pid=0x2000, usb_vid=0x0483, mask=None):
        if side is not None and serial_number is not None:
            raise ValueError("`side` and `serial_number` are mutually exclusive")
        if side is not None:
            side_enum = _core.Hand.Side.Left if side == 'left' \
                else _core.Hand.Side.Right if side == 'right' \
                else None
            if side_enum is None:
                raise ValueError(f"side must be 'left' or 'right', got {side!r}")
            super().__init__(side=side_enum, usb_pid=usb_pid, usb_vid=usb_vid, mask=mask)
        else:
            super().__init__(serial_number, usb_pid, usb_vid, mask)
```

类型存根 `update_stubs.py` 跑一次同步即可。

## 异常路径

| 场景 | 结果 |
|------|------|
| 单只手在场，side 命中 | 正常构造成功 |
| 单只手在场，side 反 | `ConnectionError("No <side> hand found; saw 1 device: SN_A; use serial_number...")` |
| 没插手 | `ConnectionError("No device found for VID=... PID=...")` |
| 左右各一只，正常使用 | 各自命中 |
| 同侧 2 只 | `ConnectionError("Multiple <side> hands found; saw 2 devices: SN_A SN_B; use serial_number to disambiguate")` |
| 某只设备 probe 时被占用/SDO 超时 | 该只跳过，继续 probe 其它；最终诊断 message 包含 probe failures |
| side + serial_number 同传 | Python 侧抛 `ValueError`（不进 C++）|
| side 字符串非法（如 `'l'`） | Python 侧抛 `ValueError` |

## 风险与未决项

| 项 | 状态 | 行动 |
|----|------|------|
| `0x5090` 的最低固件版本 | **未决** | PR review 阶段请固件团队 confirm，最坏情况是给 SDK 加固件版本判定，老固件直接报"firmware does not expose handedness" |
| `Side` enum 值 (0=Right, 1=Left) | **已确认** | 见 `docs/external/en/api-reference.mdx:228` —— `read_handedness ... (0=right, 1=left)`，与触觉模块约定相反 |
| 同进程并发 `Hand(side='left')` 和 `Hand(side='right')` | **OK** | 每个 Handler 自己 `libusb_init` 拿独立 context；libusb 的 device 级互斥由 `claim_interface` 保证 |
| 探测中途 USB 拔出 | **OK** | RAII 链完整（`Usb::~Usb` 已处理 release_interface + close + thread join），probe handler 析构干净 |
| `list_matching_serial_numbers` 失败（libusb_init 出错等） | 抛 `ConnectionError`（与现有路径一致）|

## 验收标准

1. `Hand(side='left')` / `Hand(side='right')` 在单只手场景能正常工作
2. 左+右各一只时，分别构造能命中正确设备
3. 没插手时 → `ConnectionError`，message 含 "No device found"
4. 同侧两只 → `ConnectionError`，message 含 "Multiple ... use serial_number"
5. side 反 → `ConnectionError`，message 含 "No <side> hand found"
6. `Hand(side='left', serial_number='X')` → `ValueError`
7. `Hand(side='lefty')` → `ValueError`
8. 旧 `Hand(serial_number='X')` 和 `Hand()` 路径行为不变（回归测试）
9. probe 整体耗时（双设备场景）≤ 1s

## 实现分解

| Step | 文件 | 工作量估计 |
|------|------|-----------|
| 1 | `wujihandcpp/transport/` 新增 `list_matching_serial_numbers` | S |
| 2 | `wujihandcpp/device/hand.hpp` 加 Side enum 和 probe_handedness 静态函数 | M |
| 3 | `src/main.cpp` 注册 Side enum + init overload | S |
| 4 | `src/wujihandpy/__init__.py` 加 side 参数 + 互斥校验 | S |
| 5 | `update_stubs.py` 同步类型存根 | XS（自动）|
| 6 | 手动验证（单只/双只/同侧两只/无设备）| M |
| 7 | （可选）单元/集成测试 | M |

详细的实现顺序、commit 拆分、测试 case 编排放到 wuji-plan 阶段。
