import wujihandpy
import time
import argparse
import os

parser = argparse.ArgumentParser(description="实时读取灵巧手 effort 数据（可手动推动）")
parser.add_argument("-f", "--freq", type=float, default=100, help="读取频率 (Hz), 默认 100Hz")
args = parser.parse_args()

hand = wujihandpy.Hand()

# 读取 effort limit 用于计算百分比
effort_limit = hand.read_joint_effort_limit()

# 启用所有关节（电机通电才能测量 effort）
hand.write_joint_enabled(True)

interval = 1.0 / args.freq
finger_names = ["拇指", "食指", "中指", "无名指", "小指"]

# 清屏
os.system("clear")

try:
    # 使用实时控制器读取 effort 数据
    # cutoff_freq=100Hz: 高截止频率使跟随更快，手指更容易推动
    with hand.realtime_controller(
        enable_upstream=True, filter=wujihandpy.filter.LowPass(cutoff_freq=100.0)
    ) as controller:
        while True:
            start_time = time.perf_counter()

            # 实时读取关节位置和 effort (5x4 array)
            positions = controller.get_joint_actual_position()
            effort = controller.get_joint_actual_effort()
            effort_pct = effort / effort_limit * 100

            # 跟随模式：将当前位置设为目标位置，使手可以被推动
            controller.set_joint_target_position(positions)

            # 移动光标到起始位置
            print("\033[H", end="")

            # 打印标题
            print(f"实时 Effort 读取 - 频率: {args.freq}Hz (按 Ctrl+C 停止)")
            print("提示: 跟随模式，推动灵巧手观察 effort 变化")
            print("=" * 85)
            print(f"{'手指':<8} {'位置 [J1, J2, J3, J4]':<35} {'Effort % [J1, J2, J3, J4]':<35}")
            print("-" * 85)

            # 按手指分组输出
            for i, name in enumerate(finger_names):
                pos = positions[i]
                eff = effort_pct[i]
                pos_str = f"[{pos[0]:7.2f}, {pos[1]:7.2f}, {pos[2]:7.2f}, {pos[3]:7.2f}]"
                eff_str = f"[{eff[0]:6.1f}%, {eff[1]:6.1f}%, {eff[2]:6.1f}%, {eff[3]:6.1f}%]"
                print(f"{name:<8} {pos_str:<35} {eff_str:<35}")

            print("-" * 85)

            # 精确控制读取间隔
            elapsed = time.perf_counter() - start_time
            sleep_time = max(0, interval - elapsed)
            time.sleep(sleep_time)

except KeyboardInterrupt:
    pass
finally:
    hand.write_joint_enabled(False)
    print("\n已停止实时读取，关节已禁用")
