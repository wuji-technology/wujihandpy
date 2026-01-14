import wujihandpy
import time
import argparse

parser = argparse.ArgumentParser(description="实时持续读取灵巧手数据")
parser.add_argument("-f", "--freq", type=float, default=10, help="读取频率 (Hz), 默认 10Hz")
args = parser.parse_args()

hand = wujihandpy.Hand()

interval = 1.0 / args.freq
print(f"开始实时持续读取... 频率: {args.freq}Hz")
print("按 Ctrl+C 停止")
print("-" * 40)

try:
    while True:
        start_time = time.perf_counter()

        # 实时读取输入电压
        voltage = hand.read_input_voltage()

        # 实时读取电机温度 (5x4 array)
        temps = hand.read_joint_temperature()

        # 打印实时数据
        print(f"\r电压: {voltage:.2f}V | 温度: {temps.flatten()}", end="")

        # 精确控制读取间隔
        elapsed = time.perf_counter() - start_time
        sleep_time = max(0, interval - elapsed)
        time.sleep(sleep_time)

except KeyboardInterrupt:
    print("\n\n已停止实时读取")
