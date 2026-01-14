"""
realtime_controller 模式验证脚本

前置条件：
- 设备已连接

用例步骤与预期结果：
1. 使用 with hand.realtime_controller(enable_upstream=True) 进入实时模式
2. 调用 get_joint_actual_effort() 获取 effort 数据
3. 预期：正常返回数据，无异常
"""

import sys
import wujihandpy
import numpy as np


def main():
    print("=" * 60)
    print("realtime_controller 模式验证")
    print("=" * 60)

    # 连接设备
    print("\n[步骤 1] 连接设备...")
    hand = wujihandpy.Hand()
    print("  设备连接成功")

    try:
        # 进入实时模式
        print("\n[步骤 2] 进入实时模式 (enable_upstream=True)...")
        # Use default LowPass filter
        default_filter = wujihandpy.filter.LowPass(10.0)
        with hand.realtime_controller(enable_upstream=True, filter=default_filter) as controller:
            print("  成功进入实时模式")

            # 获取 effort 数据
            print("\n[步骤 3] 调用 get_joint_actual_effort()...")
            effort = controller.get_joint_actual_effort()

            # 验证返回数据
            print("\n[验证结果]")
            print("-" * 40)

            # 检查返回类型
            is_ndarray = isinstance(effort, np.ndarray)
            print(f"  返回类型: {type(effort).__name__} (预期: ndarray) - {'PASS' if is_ndarray else 'FAIL'}")

            # 检查数据形状 (5x4: 5个手指 x 4个关节)
            expected_shape = (5, 4)
            shape_correct = effort.shape == expected_shape
            print(f"  数据形状: {effort.shape} (预期: {expected_shape}) - {'PASS' if shape_correct else 'FAIL'}")

            # 检查数据类型
            is_float64 = effort.dtype == np.float64
            print(f"  数据类型: {effort.dtype} (预期: float64) - {'PASS' if is_float64 else 'FAIL'}")

            # 打印 effort 数据
            np.set_printoptions(precision=4, suppress=True)
            print(f"\n  Effort 数据 (5x4):\n{effort}")

            # 多次读取验证稳定性
            print("\n[步骤 4] 连续读取 5 次验证稳定性...")
            for i in range(5):
                effort_i = controller.get_joint_actual_effort()
                print(f"  第 {i + 1} 次读取成功, F2J1 effort: {effort_i[1, 0]:.4f}")

            # 总结
            all_pass = is_ndarray and shape_correct and is_float64
            print("\n" + "=" * 60)
            print(f"验证结果: {'PASS' if all_pass else 'FAIL'}")
            print("=" * 60)

            return all_pass

    except Exception as e:
        print(f"\n[ERROR] 验证失败: {e}")
        print("=" * 60)
        print("验证结果: FAIL")
        print("=" * 60)
        return False


if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)
