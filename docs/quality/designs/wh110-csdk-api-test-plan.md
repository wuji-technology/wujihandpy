# WH110 相关 C SDK 接口（Part3）测试方案

| 版本 | 日期 | 作者 | 变更说明 |
|------|------|------|----------|
| v1.0 | 2026-07-01 | 林觉圣 | 初版（前瞻性设计，测试用例依赖最终 PR 落地的实际 C API 签名与 joint_command/joint_state 语义） |

关联工作项：[7026718188](https://project.feishu.cn/68e72ecbf9d6e3a06e5fe67a/workitem/7026718188)

> 本工作项仍处「开发中」，PR 未出、实现未落地。本方案为前瞻性设计：所有 AC-ID 的通过标准均以「最终 PR 暴露的 C 头文件签名与 wujihand2 aos 对齐后的 joint_command/joint_state 语义」为准，落测前需用 PR 实测签名回填本方案的占位接口名。

## 1. 测试范围

测试目标：证明 WH110 相关 C SDK 接口在行为上与 Python 侧等价，且 joint_command / joint_state 对齐 wujihand2 aos 最新内容，并能在真实 WH110 设备上端到端跑通。

非目标：不验证 WH120（zenoh@7447 / wuji-proto v98 新栈）相关接口；不做固件本体功能验证；不做性能/压测基准；不做 pybind11 绑定层本身的重构回归（绑定层为 Python 侧既有资产，本 story 仅在其上新增 C 头文件消费入口）。

版本锚定：本方案锚定到工作项 7026718188 最终合入 PR 暴露的 C API 构建（C++20，wujihandcpp 静态/动态库），以及该 PR 对齐的 wujihand2 aos joint_command/joint_state 版本。超出此构建的内容不在范围。

| 范围项 | 覆盖内容 | 边界/不覆盖内容 | 依据 |
|--------|----------|----------------|------|
| C API 接口表面 | 新增面向 C 消费者的头文件（当前 `wujihandcpp/include/wujihandcpp/utility/api.hpp` 仅含 DLL export 宏，无接口声明）；Hand 构造、设备枚举/连接、Finger/Joint 设备模型访问的 C 可调用入口 | 不覆盖头文件内部实现、链接产物体积、ABI 稳定性承诺 | 验收标准「输出 WujiHand C api，完全对齐 Python 侧」 |
| Python 等价性 | C API 每个能力与 Python 侧（pybind11 暴露的 Hand/Finger/Joint/IController）行为一致：相同输入产生相同可观察结果（连接成功/失败、读回位置值、使能后关节响应） | 不要求逐字节位级一致；不覆盖 Python 侧未暴露的内部接口 | 验收标准「完全对齐 Python 侧」 |
| joint_command 接口 | 新增 joint_command 概念的 C 接口，语义对齐 wujihand2 aos（命令通道，区分于既有 RPDO target_position/control_mode/enabled/effort_limit/reset_error 命令集） | 不覆盖固件对 joint_command 的内部解析；不覆盖 aos 本身的行为 | 验收标准「joint_command 对齐最新 wujihand2 aos 内容」 |
| joint_state 接口 | joint_state 的 C 接口与语义对齐 wujihand2 aos（当前代码库 joint_states 仅作 Zenoh bridge SUB publisher，`sensor_msgs/JointState` 扁平行优先投影 joint/actual_position；需对齐 aos 而非仅 bridge 投影） | 不覆盖 Zenoh bridge 的发布链路本身（既有资产）；不覆盖 ROS schema 兼容 | 验收标准「joint_state 对齐最新 wujihand2 aos 内容」 |
| WH110 设备可达性 | 真实 WH110 设备（STM32G431 joint × 20 + STM32H7 sboard + STM32H723 tboard，老固件栈）能被 C API 发现、连接并完成读写循环 | 不覆盖 WH110 固件升级/烧录流程；不覆盖 USB PID/VID 冲突的回归（既有默认 pid 已从 -1 改 0x2000，属已修资产） | 代码结构：Hand 构造默认 usb_pid=0x2000、usb_vid=0x0483；WH110 vs WH120 设备区分 |

## 2. 核心验收点

> 工作项未提供结构化、可逐条勾选的验收标准，下列 AC-ID 由「输出 WujiHand C api 对齐 Python 侧」「joint_command/joint_state 对齐 wujihand2 aos」高层验收文案分解得出。

| AC-ID | 验收点 | 通过标准 | 优先级 |
|-------|--------|----------|--------|
| AC-01 | C API 头文件接口表面存在且对齐 Python 侧能力 | 新增 C 头文件可被独立 C 消费者 `#include` 并链接通过；暴露的接口集合覆盖 Python 侧 Hand/Finger/Joint/IController 的公开能力（连接、设备模型访问、读写、使能、错误复位），每项能力在 C 侧均有对应可调用入口 | P0 |
| AC-02 | Hand 构造与设备连接 C/Python 等价 | 用相同 serial_number / usb_pid=0x2000 / usb_vid=0x0483 调 C 接口与 Python 接口，对同一台 WH110 设备的发现与连接结果一致（同连上或同连不上，连接成功后能读到相同的设备标识） | P0 |
| AC-03 | Finger/Joint 设备模型访问 C/Python 等价 | 通过 C API 取到的 5 指 × 4 关节设备模型结构与 Python 侧一致；对同一关节读回的实际位置/状态在相同时间窗内一致（相对基线判断，不引入未给定的数值阈值） | P1 |
| AC-04 | joint_command 接口存在且语义对齐 wujihand2 aos | C 侧存在 joint_command 接口；按 wujihand2 aos 定义的方式下发命令后，关节的可观察行为（运动到目标、停住、报错等）与 aos 语义一致；命令通道与既有 RPDO target_position 等命令集的关系按 aos 定义区分 | P0 |
| AC-05 | joint_state 接口存在且语义对齐 wujihand2 aos | C 侧存在 joint_state 接口；上报内容（关节位置等可观测量、字段集合、顺序、单位）按 wujihand2 aos 定义，而非沿用 Zenoh bridge 现有的扁平行优先投影；连续读取无丢帧/卡死（人工观察结论，不引入未给定的 Hz 阈值） | P0 |
| AC-06 | WH110 真实设备端到端跑通 | 在真实 WH110 设备上用 C API 完成「连接 → 建立设备模型 → 使能 → 下发 joint_command → 读 joint_state → 断开」全链路，无异常退出、无资源泄漏（重复连接-断开若干次后仍正常） | P0 |
| AC-07 | USB PID/VID 默认值与 TactileGlove 不冲突 | C API 默认 usb_pid=0x2000 / usb_vid=0x0483，与共享 VID 0x0483 的 TactileGlove 设备枚举互不串扰（同插两类设备时各自被正确识别） | P1 |

## 3. 风险到测试覆盖矩阵

| 风险/缺口 | 对应 AC-ID | 测试层级/方法 | 执行方式 | 通过标准 |
|-----------|------------|---------------|----------|----------|
| 实现/PR 未落地，C API 签名未知，joint_command/joint_state 对 aos 的对齐口径未确定 | AC-01,04,05,06 | 阻塞项 + 等价性测试 / E2E | 待 PR 合入后人工核对签名回填，再用自动化 E2E | 阻塞：PR 落地前 AC-04/05 无法实测，仅能做设计评审；落地后须先人工核对 C 头文件签名与 aos 定义一致，方可进入设备测试 |
| C API 与 Python 侧行为不一致（字段顺序、单位、错误返回差异） | AC-02,03 | 等价性测试 / 集成 | 真实 WH110 设备上 C/Python 双跑同一序列，自动化比对读回值 | 引用 AC-02/03：相同输入同连/同断、读回值在相对基线内一致 |
| joint_state 语义沿用旧 Zenoh bridge 扁平投影，未真正对齐 aos | AC-05 | 语义对齐验证 / 集成 | 人工核对 aos 定义字段集合 vs C 接口暴露字段；设备上读取验证 | 引用 AC-05：字段集合/顺序/单位与 aos 一致，非旧投影 |
| joint_command 与既有 RPDO 命令通道混淆或重复 | AC-04 | 语义对齐验证 / 集成 | 人工核对 aos 命令通道区分；设备上分别触发并观察关节行为差异 | 引用 AC-04：两通道按 aos 定义区分，互不误触发 |
| WH110 老固件栈（G431/H7）与新 C API 兼容性 | AC-02,06 | 兼容性 / E2E | 真实 WH110 设备本地执行 | 引用 AC-06：全链路无异常退出 |
| USB PID/VID 与 TactileGlove 共享 VID 0x0483 串扰（历史已改默认值，需防回归） | AC-07 | 兼容性 / 集成 | 同插两类设备本地枚举验证 | 引用 AC-07：各自被正确识别 |
| 跨平台链接/头文件可见性（Windows dllexport vs Linux visibility） | AC-01 | 构建/兼容性 | CI 多平台构建 | 引用 AC-01：独立 C 消费者各平台 `#include`+链接通过 |

## 4. 整体测试方法

| 方法 | 适用范围 | 使用原因 | 执行层级 |
|------|----------|----------|----------|
| 功能等价性测试 | AC-01,02,03 | 核心是 C/Python 行为对齐，须用相同输入序列双跑比对 | 集成（真实设备，C 与 Python 双跑） |
| 语义对齐验证 | AC-04,05 | joint_command/joint_state 对齐 wujihand2 aos 属定义级一致性，须先人工核对 aos 定义再设备验证 | 集成 + 人工设计评审 |
| 端到端测试 | AC-06 | 证明 WH110 真实设备上 C API 全链路可用 | E2E（本地真实设备） |
| 兼容性测试 | AC-07 | 防 USB PID/VID 串扰回归与跨平台链接 | 集成（多设备）+ CI（多平台构建） |
| 回归测试 | AC-01..07 | 防止新增 C 入口影响既有 pybind11 绑定层与 Zenoh bridge 行为 | CI（既有 tests/*.py 全绿） |

## 5. 优先级与阻塞标准

优先级原则：
- P0：AC-01（C 接口表面存在）、AC-02（连接等价）、AC-04（joint_command 对齐 aos）、AC-05（joint_state 对齐 aos）、AC-06（WH110 端到端）—— 任一不通过即阻塞交付
- P1：AC-03（设备模型访问等价）、AC-07（USB PID/VID 防回归）—— 重要，可带限制交付但须在版本内闭环
- P2：无（本 story 验收聚焦核心接口等价与对齐，不展开辅助/极端场景）

阻塞标准：
- AC-01 失败（无可用 C 头文件或链接不过）
- AC-04 或 AC-05 语义与 wujihand2 aos 不一致
- AC-06 在 WH110 真实设备上无法端到端跑通或重复连接-断开出现资源泄漏/异常退出
- PR 未落地导致 AC-04/05 的 aos 对齐口径无法确定（设计阶段阻塞，须先获取 PR 与 aos 定义）

准入标准：
- 工作项 7026718188 的 PR 已提供可编译的 C 头文件与示例消费者
- joint_command / joint_state 对 wujihand2 aos 的对齐口径已书面确认（PR 说明或设计评论）
- 至少一台可用的 WH110 真实设备（STM32G431 joint × 20 + H7 sboard + H723 tboard）
- aos joint_command/joint_state 定义文档或代码可访问

准出标准：
- AC-01..06 全部通过，AC-07 在版本内闭环
- 既有 tests/*.py 在新增 C 入口后全绿（无回归）

## 6. 环境、数据、依赖与排除项

测试环境：
- WH110 真实设备：STM32G431 joint × 20 + STM32H7 sboard + STM32H723 tboard（老固件栈，区别于 WH120 zenoh@7447）
- USB 直连主机（usb_vid=0x0483，usb_pid=0x2000）
- 对照组：同机 Python 侧 wujihandpy（pybind11 绑定，Hand/Finger/Joint/IController）
- 对照设备：TactileGlove（共享 VID 0x0483，用于 AC-07 串扰验证）
- 构建环境：C++20 / cmake / scikit-build-core；多平台（Linux + Windows）构建矩阵

测试数据：
- WH110 设备序列号（serial_number，Hand 构造入参）
- joint_command 测试目标值序列（按 aos 定义口径，待 PR 确认）
- joint_state 期望字段集合/顺序（按 aos 定义，待 PR 确认）

外部依赖：
- wujihand2 aos joint_command / joint_state 定义（PR 或文档）
- wujihandcpp C++ SDK 库（静态/动态链接产物）
- wuji-technology/wh110-firmware（设备固件，已烧录在测试设备上）

排除项：
- WH120 相关 C SDK 接口（属另一新栈）
- 固件本体功能、烧录/升级流程
- pybind11 绑定层重构、Zenoh bridge 发布链路本身（既有资产）
- 性能/压测基准（验收标准未给出数值阈值）

待确认问题：
- joint_command 的完整语义与字段集（owner：开发负责人 / aos 维护者；确认来源：PR 描述或 aos 代码/文档）—— 阻塞 AC-04 落测
- joint_state 对齐 aos 后的字段集合/顺序/单位（owner：同上）—— 阻塞 AC-05 落测
- C API 是否要求 ABI 稳定性承诺，还是仅功能对齐（owner：开发负责人）—— 影响 AC-01 通过标准边界
- 测试用 WH110 设备 SN 与可用性（owner：QA 林觉圣 / 硬件联系人）—— 阻塞 AC-06 设备段

## 兼容性矩阵

涉及硬件/固件/SDK 构建组合，追加本节。

| 维度 | 取值 | 备注 |
|------|------|------|
| 设备型号 | WH110（G431 joint + H7 sboard + H723 tboard，老固件栈） | 非 WH120（zenoh@7447 新栈） |
| SDK 构建平台 | Linux / Windows | 验证 WUJIHANDCPP_API 宏在 dllexport / visibility 两套导出机制下均可链接 |
| 对照侧 | Python wujihandpy（pybind11） | 同设备双跑用于等价性 |
| 共存设备 | TactileGlove（VID 0x0483） | AC-07 串扰验证 |

## 状态前置与拒绝语义

C API 涉及设备状态机（连接/断开、使能/失能），追加本节。预期以最终 PR 的 C API 签名为准，下表为对齐 Python 侧行为的预期口径。

| 状态 | 操作 | 预期（接受 / 拒绝及外部现象） |
|------|------|------------------------------|
| 未连接 | 下发 joint_command / 读 joint_state | 拒绝并报错，无可观察设备动作（与 Python 侧一致） |
| 已连接未使能 | 下发 joint_command | 接受但关节不动（按 aos 定义），或拒绝并报错 —— 待 PR 确认口径 |
| 已使能 | 下发 joint_command | 接受，关节运动到目标并停住 |
| 已使能 | 读 joint_state | 接受，返回 aos 定义字段集，值随关节实际位置变化 |
| 故障态（过流/ Fatal） | 下发 joint_command | 拒绝或保持不动，需 error_inject/reset_error 后恢复（与 Python 侧一致） |
| 断开中/已断开 | 任何操作 | 拒绝并报错，无资源泄漏 |

## 观测与证据

AC-04/05/06 需观测 joint_command/joint_state 是否真正生效，追加本节。

| 测试点 | 可观测量（读哪个上报量/状态，不写寄存器地址） | 判读基线（操作前/操作后/重启后三段比对） | 证据留存 |
|--------|---------------------------------------------------|------------------------------------------|----------|
| joint_command 生效（AC-04） | 关节实际位置（joint_state 上报）、关节是否运动到目标并停住 | 操作前：关节在某位置；操作后：运动到 joint_command 目标并稳定；重启后：回到默认/零点 | C 与 Python 双跑命令日志 + 关节位置回读快照 |
| joint_state 字段对齐 aos（AC-05） | joint_state 返回的字段集合/顺序/单位 | 操作前/后：字段集合与 aos 定义一致（非旧 Zenoh bridge 扁平投影）；值随关节实际位置变化 | joint_state 原始返回样例 + aos 定义对照 |
| 重复连接-断开无泄漏（AC-06） | 连接成功率、进程是否异常退出、USB 设备是否仍可枚举 | 多次连接-断开前后：连接成功率不下降、进程不崩、设备仍可重新枚举 | 连接-断开循环日志 + 末次连接成功证据 |
