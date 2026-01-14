# WujihandPy 测试框架结构

## 目录结构

```
tests/
├── __init__.py                  # 测试包初始化
├── conftest.py                  # pytest 共享配置和 fixture
├── pytest.ini                   # pytest 配置
├── test_hand.py                 # Hand 类测试
│   ├── TestHandConstructor
│   ├── TestHandReadOnlyProperties
│   ├── TestHandJointReadProperties
│   ├── TestHandJointWriteProperties
│   └── TestHandSpecialMethods
├── test_finger.py               # Finger 类测试
│   ├── TestFingerMethods
│   ├── TestFingerJointReadProperties
│   ├── TestFingerJointWriteProperties
│   └── TestFingerAllFingers
├── test_joint.py                # Joint 类测试
│   ├── TestJointReadProperties
│   ├── TestJointWriteProperties
│   ├── TestJointPosition
│   └── TestJointAllJoints
├── test_controller.py           # IController 测试
│   ├── TestIControllerBasic
│   ├── TestIControllerNoUpstream
│   ├── TestIControllerRealTimeControl
│   ├── TestIControllerWithFilter
│   └── TestIControllerPositionRange
├── test_filter.py               # filter 模块测试
│   ├── TestLowPassFilter
│   ├── TestFilterInterface
│   └── TestFilterWithHand
├── test_logging.py              # logging 模块测试
│   ├── TestLoggingLevel
│   ├── TestLoggingConsole
│   ├── TestLoggingFile
│   ├── TestLoggingLevel
│   ├── TestLoggingFlush
│   └── TestLoggingIntegration
└── test_exceptions.py           # 异常处理测试
    ├── TestTimeoutError
    ├── TestParameterErrors
    ├── TestControllerClosedError
    ├── TestInvalidMaskError
    └── TestExceptionHandling
```

## 测试优先级分类

```mermaid
graph TB
    subgraph P0 [P0 - 必须通过]
        direction TB
        P0_CON[构造函数测试]
        P0_READ[只读属性测试]
        P0_JOINT[关节读写测试]
        P0_CTRL[控制器基础测试]
    end

    subgraph P1 [P1 - 推荐通过]
        direction TB
        P1_ASYNC[异步操作测试]
        P1_FILTER[滤波器测试]
        P1_LOG[日志配置测试]
        P1_PERF[性能测试]
    end

    subgraph P2 [P2 - 可选]
        direction TB
        P2_EDGE[边界条件测试]
        P2_RAW[SDO 调试测试]
        P2_SPECIAL[特殊场景测试]
    end

    P0 --> P1 --> P2
```

## Fixture 依赖关系

```mermaid
graph LR
    subgraph Fixtures
        hand["hand\nsession 级别"]
        connected_hand["connected_hand\nfunction 级别"]
        enabled_hand["enabled_hand\n已启用关节"]
        thumb_joint["thumb_joint\n拇指关节"]
        index_finger["index_finger\n食指"]
        controller["controller\n实时控制器"]
    end

    hand --> connected_hand
    connected_hand --> enabled_hand
    connected_hand --> thumb_joint
    connected_hand --> index_finger
    connected_hand --> controller
    enabled_hand --> thumb_joint
```

## 测试用例统计

| 模块 | P0 | P1 | P2 | 总计 |
|------|----|----|----|------|
| Hand | 7 | 11 | 7 | 25 |
| Finger | 4 | 5 | 3 | 12 |
| Joint | 5 | 6 | 1 | 12 |
| IController | 5 | 5 | 2 | 12 |
| filter | 2 | 0 | 3 | 5 |
| logging | 0 | 7 | 1 | 8 |
| exceptions | 1 | 3 | 3 | 7 |
| **总计** | **24** | **37** | **20** | **81** |

## 测试流程

```mermaid
flowchart TD
    A[开始测试] --> B{设备连接?}
    B -->|是| C[创建 Hand 实例]
    B -->|否| D[跳过测试]
    C --> E{执行测试}
    E --> F{通过?}
    F -->|是| G[清理资源]
    F -->|否| H[记录失败]
    H --> I[继续其他测试]
    G --> I
    I --> J[生成测试报告]
```

## Fixture 作用域

```mermaid
graph TB
    subgraph Session ["session 级别 (整个测试会话)"]
        S1["hand\nHand 实例"]
    end

    subgraph Function ["function 级别 (每个测试函数)"]
        F1["connected_hand\n已连接设备"]
        F2["enabled_hand\n已启用关节"]
        F3["thumb_joint\n拇指关节"]
        F4["index_finger\n食指"]
        F5["controller\n实时控制器"]
    end

    S1 --> F1
    F1 --> F2
    F1 --> F3
    F1 --> F4
    F1 --> F5
    F2 --> F3
```

## 运行命令

```bash
# 运行所有测试
pytest tests/ -v

# 按优先级运行
pytest tests/ -k "P0" -v    # P0 测试
pytest tests/ -k "P1" -v    # P1 测试
pytest tests/ -k "P2" -v    # P2 测试

# 运行特定模块
pytest tests/test_hand.py -v
pytest tests/test_controller.py -v

# 生成 JUnit XML 报告
pytest tests/ --junitxml=report.xml

# 生成覆盖率报告
pytest tests/ --cov=wujihandpy --cov-report=html
```

## 测试标记 (Markers)

```python
@pytest.mark.P0  # 必须通过 - 基础功能
@pytest.mark.P1  # 推荐通过 - 完整功能
@pytest.mark.P2  # 可选 - 边界条件
```

## conftest.py 核心配置

```python
# Fixture 作用域
@pytest.fixture(scope="session")    # 整个会话共享
@pytest.fixture(scope="function")   # 每个函数独立

# 共享 Fixture
hand                    # Hand 实例 (session)
connected_hand          # 已连接设备 (function)
enabled_hand            # 已启用关节 (function)
thumb_joint             # 拇指关节 (function)
index_finger            # 食指 (function)
valid_position_array    # 有效位置数组 (5,4)
valid_single_value      # 有效单值 (float)
```

## 异常测试覆盖

```mermaid
graph TB
    subgraph TimeoutError ["TimeoutError 测试"]
        T1[同步操作超时]
        T2[异步操作超时]
        T3[超时消息验证]
    end

    subgraph ParameterError ["参数错误测试"]
        P1[数组维度错误]
        P2[数组形状不匹配]
        P3[索引越界]
    end

    subgraph ControllerError ["控制器错误测试"]
        C1[关闭后操作]
        C2[重复关闭]
    end

    TimeoutError --> ParameterError --> ControllerError
```

## 实时控制器测试

```mermaid
sequenceDiagram
    participant T as 测试
    participant H as Hand
    participant C as IController

    T->>H: write_joint_enabled(True)
    T->>H: realtime_controller(enable_upstream=True, filter=LowPass)
    H->>C: 创建控制器
    T->>C: get_joint_actual_position()
    T->>C: get_joint_actual_effort()
    T->>C: set_joint_target_position(array)
    Note over T,C: 1kHz 循环
    T->>C: close()
    T->>H: write_joint_enabled(False)
```
