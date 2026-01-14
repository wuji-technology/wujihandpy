"""
Effort 数据 1kHz 同步更新验证脚本

前置条件：
- 设备处于实时模式，enable_upstream=True

验证内容：
- 连续采集 effort 和 position 数据
- 验证 effort 与 position 数据同步更新
- 验证更新频率约 1kHz

运动模式：
1. periodic  - 周期屈伸：手指做正弦波形的周期性屈伸运动
2. static    - 保持静止：手保持当前位置不动，仅读取数据
3. external  - 施加外力：手保持位置，由测试人员手动施加外力

支持单/双灵巧手：
- 不指定序列号时自动连接第一个设备
- 通过 --sn 参数指定一个或两个序列号
"""

from __future__ import annotations

import math
import time
from collections import deque
from dataclasses import dataclass
from datetime import datetime
from enum import Enum
from pathlib import Path
from typing import Optional

import numpy as np
import wujihandpy

from _utils import (
    HandInfo,
    connect_hands,
    create_arg_parser,
    disable_all_hands,
    enable_all_hands,
    print_summary,
)


class MotionMode(Enum):
    PERIODIC = "periodic"  # 周期屈伸
    STATIC = "static"  # 保持静止
    EXTERNAL = "external"  # 施加外力


@dataclass
class Sample:
    """单次采样数据"""

    timestamp: float  # 采样时间戳 (秒)
    effort: np.ndarray  # 5x4 effort 数据
    position: np.ndarray  # 5x4 position 数据


@dataclass
class UpdateEvent:
    """数据更新事件"""

    timestamp: float
    effort_changed: bool
    position_changed: bool


def arrays_differ(a: np.ndarray, b: np.ndarray, threshold: float = 1e-9) -> bool:
    """检查两个数组是否有差异"""
    return np.any(np.abs(a - b) > threshold)


def run_verification_single(
    hand_info: HandInfo,
    mode: MotionMode = MotionMode.PERIODIC,
    duration: float = 5.0,
    sample_interval: float = 0.0001,  # 100us 采样间隔，远小于 1ms
    log_dir: Optional[str] = None,
) -> dict:
    """
    对单只手运行验证测试

    Args:
        hand_info: 设备信息
        mode: 运动模式
        duration: 测试持续时间（秒）
        sample_interval: 采样间隔（秒）
        log_dir: 日志目录，None 时使用默认目录

    Returns:
        验证结果字典
    """
    hand = hand_info.hand
    hand_name = hand_info.name

    mode_names = {
        MotionMode.PERIODIC: "周期屈伸",
        MotionMode.STATIC: "保持静止",
        MotionMode.EXTERNAL: "施加外力",
    }

    # 创建日志文件
    if log_dir is None:
        log_dir = Path.home() / ".wuji" / "test" / "logs"
    else:
        log_dir = Path(log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)

    timestamp_str = datetime.now().strftime("%Y%m%d_%H%M%S")
    safe_name = hand_name.replace(" ", "_")
    log_file = log_dir / f"effort_sync_{safe_name}_{timestamp_str}.log"

    print(f"\n[{hand_name}] 运动模式: {mode_names[mode]}")
    print(f"[{hand_name}] 开始采集数据，持续 {duration} 秒...")
    print(f"[{hand_name}] 采样间隔: {sample_interval * 1e6:.1f} us")
    print(f"[{hand_name}] 日志文件: {log_file}")

    if mode == MotionMode.EXTERNAL:
        print(f"\n[{hand_name}] 请准备对手指施加外力...")
        time.sleep(2)
        print(f"[{hand_name}] 开始采集！")

    # 打印设置
    np.set_printoptions(precision=3, suppress=True, linewidth=120)
    print_interval = 0.5  # 每 500ms 打印一次（多手时减少打印频率）
    last_print_time = 0.0
    log_interval = 0.001  # 每 1ms 记录一次日志
    last_log_time = 0.0

    # 使用 deque 存储采样数据，限制大小避免内存问题
    max_samples = int(duration / sample_interval) + 1000
    samples: deque[Sample] = deque(maxlen=max_samples)
    update_events: list[UpdateEvent] = []

    # 周期运动参数
    motion_freq = 0.5  # 0.5 Hz，2秒一个周期
    motion_amplitude = 0.8  # 运动幅度 (0-1)

    # 进入实时模式
    with hand.realtime_controller(
        enable_upstream=True,
        filter=wujihandpy.filter.LowPass(cutoff_freq=100.0),  # 高截止频率以减少滤波延迟
    ) as controller, open(log_file, "w") as log_f:
        # 写入日志头
        log_f.write(f"# Effort Sync Verification Log\n")
        log_f.write(f"# Hand: {hand_name} (SN: {hand_info.serial_number})\n")
        log_f.write(f"# Mode: {mode.value} ({mode_names[mode]})\n")
        log_f.write(f"# Duration: {duration}s\n")
        log_f.write(f"# Start: {datetime.now().isoformat()}\n")
        log_f.write(f"# Format: timestamp,position[5x4],effort[5x4]\n")
        log_f.write("#" + "=" * 79 + "\n")

        # 初始采样
        prev_effort = controller.get_joint_actual_effort().copy()
        prev_position = controller.get_joint_actual_position().copy()

        start_time = time.perf_counter()
        end_time = start_time + duration
        sample_count = 0
        static_target = None

        while time.perf_counter() < end_time:
            now = time.perf_counter()
            elapsed_time = now - start_time

            # 根据模式设置目标位置
            if mode == MotionMode.PERIODIC:
                # 周期屈伸运动
                y = (1 - math.cos(2 * math.pi * motion_freq * elapsed_time)) / 2
                y *= motion_amplitude
                target = np.array(
                    [
                        [0, 0, 0, 0],  # F1 拇指不动
                        [y, 0, y, y],  # F2 食指
                        [y, 0, y, y],  # F3 中指
                        [y, 0, y, y],  # F4 无名指
                        [y, 0, y, y],  # F5 小指
                    ],
                    dtype=np.float64,
                )
                controller.set_joint_target_position(target)
            elif mode == MotionMode.STATIC:
                # 保持静止 - 设置目标为当前位置
                if static_target is None:
                    static_target = controller.get_joint_actual_position().copy()
                controller.set_joint_target_position(static_target)
            # EXTERNAL 模式不设置目标位置，保持当前控制

            # 获取当前数据
            effort = controller.get_joint_actual_effort()
            position = controller.get_joint_actual_position()

            # 记录采样
            samples.append(
                Sample(
                    timestamp=elapsed_time,
                    effort=effort.copy(),
                    position=position.copy(),
                )
            )

            # 定期写入日志
            if elapsed_time - last_log_time >= log_interval:
                last_log_time = elapsed_time
                # 将数组展平为一行
                pos_flat = ",".join(f"{v:.6f}" for v in position.flatten())
                eff_flat = ",".join(f"{v:.6f}" for v in effort.flatten())
                log_f.write(f"{elapsed_time:.6f},{pos_flat},{eff_flat}\n")

            # 定期打印
            if elapsed_time - last_print_time >= print_interval:
                last_print_time = elapsed_time
                print(
                    f"  [{hand_name}] {elapsed_time:.1f}s / {duration:.1f}s, "
                    f"更新: {len(update_events)}"
                )

            # 检测数据更新
            effort_changed = arrays_differ(effort, prev_effort)
            position_changed = arrays_differ(position, prev_position)

            if effort_changed or position_changed:
                update_events.append(
                    UpdateEvent(
                        timestamp=elapsed_time,
                        effort_changed=effort_changed,
                        position_changed=position_changed,
                    )
                )
                prev_effort = effort.copy()
                prev_position = position.copy()

            sample_count += 1

            # 控制采样频率
            loop_elapsed = time.perf_counter() - now
            if loop_elapsed < sample_interval:
                time.sleep(sample_interval - loop_elapsed)

        actual_duration = time.perf_counter() - start_time

        # 写入日志结束信息
        log_f.write("#" + "=" * 79 + "\n")
        log_f.write(f"# End: {datetime.now().isoformat()}\n")
        log_f.write(f"# Actual duration: {actual_duration:.3f}s\n")
        log_f.write(f"# Total samples: {sample_count}\n")
        log_f.write(f"# Update events: {len(update_events)}\n")

    print(f"\n[{hand_name}] 采集完成，实际持续 {actual_duration:.2f} 秒")
    print(f"[{hand_name}] 总采样数: {sample_count}")
    print(f"[{hand_name}] 检测到更新事件数: {len(update_events)}")
    print(f"[{hand_name}] 日志已保存: {log_file}")

    return analyze_results(samples, update_events, actual_duration, log_file, hand_name)


def analyze_results(
    samples: deque[Sample],
    update_events: list[UpdateEvent],
    duration: float,
    log_file: Optional[Path] = None,
    hand_name: str = "",
) -> dict:
    """分析验证结果"""
    prefix = f"[{hand_name}] " if hand_name else ""

    print(f"\n{prefix}" + "=" * 50)
    print(f"{prefix}验证结果分析")
    print(f"{prefix}" + "=" * 50)

    result = {
        "hand_name": hand_name,
        "total_samples": len(samples),
        "total_updates": len(update_events),
        "duration": duration,
        "sync_verified": False,
        "frequency_verified": False,
        "log_file": str(log_file) if log_file else None,
    }

    if len(update_events) < 2:
        print(f"{prefix}警告: 更新事件太少，无法进行有效分析")
        return result

    # 1. 分析同步性
    print(f"\n{prefix}[1] 同步性分析")
    print(f"{prefix}" + "-" * 40)

    both_updated = sum(
        1 for e in update_events if e.effort_changed and e.position_changed
    )
    only_effort = sum(
        1 for e in update_events if e.effort_changed and not e.position_changed
    )
    only_position = sum(
        1 for e in update_events if not e.effort_changed and e.position_changed
    )

    print(f"{prefix}  effort 和 position 同时更新: {both_updated} 次")
    print(f"{prefix}  仅 effort 更新: {only_effort} 次")
    print(f"{prefix}  仅 position 更新: {only_position} 次")

    sync_ratio = both_updated / len(update_events) if update_events else 0
    print(f"{prefix}  同步率: {sync_ratio * 100:.1f}%")

    # 同步率 > 95% 视为同步
    result["sync_verified"] = sync_ratio > 0.95
    result["sync_ratio"] = sync_ratio

    # 2. 分析更新频率
    print(f"\n{prefix}[2] 更新频率分析")
    print(f"{prefix}" + "-" * 40)

    # 计算更新间隔
    intervals = []
    for i in range(1, len(update_events)):
        interval = update_events[i].timestamp - update_events[i - 1].timestamp
        intervals.append(interval)

    if intervals:
        intervals_array = np.array(intervals)
        mean_interval = np.mean(intervals_array)
        std_interval = np.std(intervals_array)
        min_interval = np.min(intervals_array)
        max_interval = np.max(intervals_array)
        update_freq = 1.0 / mean_interval if mean_interval > 0 else 0

        print(f"{prefix}  平均更新间隔: {mean_interval * 1000:.3f} ms")
        print(f"{prefix}  标准差: {std_interval * 1000:.3f} ms")
        print(f"{prefix}  最小间隔: {min_interval * 1000:.3f} ms")
        print(f"{prefix}  最大间隔: {max_interval * 1000:.3f} ms")
        print(f"{prefix}  更新频率: {update_freq:.1f} Hz")

        result["mean_interval_ms"] = mean_interval * 1000
        result["std_interval_ms"] = std_interval * 1000
        result["update_frequency_hz"] = update_freq

        # 验证频率是否接近 1kHz (允许 ±10% 误差)
        result["frequency_verified"] = 900 <= update_freq <= 1100

    # 3. 频率分布直方图 (文本形式)
    print(f"\n{prefix}[3] 更新间隔分布")
    print(f"{prefix}" + "-" * 40)

    if intervals:
        # 将间隔分成 10 个区间
        hist, bin_edges = np.histogram(intervals_array * 1000, bins=10)
        max_count = max(hist) if max(hist) > 0 else 1

        for i in range(len(hist)):
            bar_len = int(hist[i] / max_count * 40)
            bar = "#" * bar_len
            print(
                f"{prefix}  {bin_edges[i]:6.2f}-{bin_edges[i + 1]:6.2f} ms: {bar} ({hist[i]})"
            )

    # 4. 总结
    print(f"\n{prefix}" + "=" * 50)
    print(f"{prefix}验证总结")
    print(f"{prefix}" + "=" * 50)

    if result["sync_verified"]:
        print(f"{prefix}  [PASS] effort 与 position 数据同步更新")
    else:
        print(f"{prefix}  [FAIL] effort 与 position 数据未能同步更新")

    if result["frequency_verified"]:
        print(
            f"{prefix}  [PASS] 更新频率约 1kHz ({result.get('update_frequency_hz', 0):.1f} Hz)"
        )
    else:
        print(
            f"{prefix}  [FAIL] 更新频率不符合预期 ({result.get('update_frequency_hz', 0):.1f} Hz)"
        )

    overall = result["sync_verified"] and result["frequency_verified"]
    result["passed"] = overall
    print(f"\n{prefix}  整体验证结果: {'PASS' if overall else 'FAIL'}")

    return result


def select_mode() -> MotionMode:
    """交互式选择运动模式"""
    print("\n请选择运动模式:")
    print("  1. periodic  - 周期屈伸：手指做正弦波形的周期性屈伸运动")
    print("  2. static    - 保持静止：手保持当前位置不动，仅读取数据")
    print("  3. external  - 施加外力：手保持位置，由测试人员手动施加外力")

    while True:
        choice = input("\n请输入选项 (1/2/3) [默认: 1]: ").strip()
        if choice == "" or choice == "1":
            return MotionMode.PERIODIC
        elif choice == "2":
            return MotionMode.STATIC
        elif choice == "3":
            return MotionMode.EXTERNAL
        else:
            print("无效选项，请重新输入")


def main(
    mode: Optional[MotionMode] = None,
    duration: float = 5.0,
    serial_numbers: Optional[list[str]] = None,
):
    """
    主函数

    Args:
        mode: 运动模式，None 时交互式选择
        duration: 测试持续时间（秒）
        serial_numbers: 灵巧手序列号列表
    """
    print("=" * 60)
    print("Effort 数据 1kHz 同步更新验证")
    print("=" * 60)

    # 选择运动模式
    if mode is None:
        mode = select_mode()

    # 连接设备
    print("\n[步骤 1] 连接设备...")
    hands = connect_hands(serial_numbers)
    print(f"  共连接 {len(hands)} 只灵巧手")

    try:
        # 启用关节
        print("\n[步骤 2] 启用所有关节...")
        enable_all_hands(hands)

        # 对每只手运行验证
        results: dict[str, bool] = {}
        for hand_info in hands:
            result = run_verification_single(
                hand_info, mode=mode, duration=duration
            )
            results[hand_info.name] = result.get("passed", False)

        # 打印总结
        all_pass = print_summary(results, "Effort 同步验证")
        return all_pass

    finally:
        # 禁用关节
        print("\n禁用所有关节...")
        disable_all_hands(hands)


if __name__ == "__main__":
    parser = create_arg_parser("Effort 数据 1kHz 同步更新验证")
    parser.add_argument(
        "-m",
        "--mode",
        type=str,
        choices=["periodic", "static", "external"],
        help="运动模式: periodic(周期屈伸), static(保持静止), external(施加外力)",
    )
    parser.add_argument(
        "-d",
        "--duration",
        type=float,
        default=5.0,
        help="测试持续时间（秒），默认 5.0",
    )

    args = parser.parse_args()

    mode = MotionMode(args.mode) if args.mode else None
    success = main(
        mode=mode,
        duration=args.duration,
        serial_numbers=args.serial_numbers,
    )
    exit(0 if success else 1)
