"""
write_joint_effort_limit 功能验证脚本

前置条件：
- Python SDK 已安装
- 设备已连接

用例步骤与预期结果：
1. 调用 write_joint_effort_limit() 方法设置限制值
2. 预期：方法正常执行，设置生效
"""

import sys
import wujihandpy
import numpy as np
import time


def main():
    print("=" * 60)
    print("write_joint_effort_limit 功能验证")
    print("=" * 60)

    # 连接设备
    print("\n[步骤 1] 连接设备...")
    try:
        hand = wujihandpy.Hand()
        print("  设备连接成功")
    except Exception as e:
        print(f"  [ERROR] 连接设备失败: {e}")
        return False

    try:
        # 1. 读取当前 Effort Limit (备份)
        print("\n[步骤 2] 读取当前 Effort Limit (作为备份)...")
        original_limits = hand.read_joint_effort_limit()
        print(f"  当前 Effort Limit:\n{original_limits}")

        # 2. 设置新的 Effort Limit
        # 设定一个安全的测试值，例如 0.5A (500mA) 或 1.0A (1000mA)
        # 注意：如果原始值很小，不要设置得过大；如果原始值很大，不要设置得过小以免影响操作（虽然后面会恢复）
        # 这里统一设置为 1.0A 进行测试
        test_limit_value = 1.0
        test_limits = np.full((5, 4), test_limit_value, dtype=np.float64)

        print(f"\n[步骤 3] 设置新的 Effort Limit 为全是 {test_limit_value} A ...")
        hand.write_joint_effort_limit(test_limits)
        print("  写入命令执行完成")
        
        # 稍作等待确保生效（虽然 write 通常是同步或阻塞直到确认）
        time.sleep(0.1)

        # 3. 再次读取并验证
        print("\n[步骤 4] 读取并验证设置是否生效...")
        current_limits = hand.read_joint_effort_limit()
        print(f"  读取到的 Effort Limit:\n{current_limits}")

        # 验证差异
        # Firmware存储单位为mA (uint16)，精度为 0.001A
        # 允许误差 0.0015 以防浮点转换误差
        diff = np.abs(current_limits - test_limits)
        max_diff = np.max(diff)
        print(f"  最大误差: {max_diff:.6f} A")

        is_success = max_diff < 0.002
        if is_success:
            print("  [PASS] 设置生效 (误差在允许范围内)")
        else:
            print(f"  [FAIL] 设置未生效或误差过大 (期望: {test_limit_value}, 实际最大偏差: {max_diff})")

    except Exception as e:
        print(f"\n[ERROR] 测试过程中发生异常: {e}")
        is_success = False
    
    finally:
        # 4. 恢复原始值
        if 'hand' in locals() and 'original_limits' in locals():
            print("\n[步骤 5] 恢复原始 Effort Limit...")
            try:
                hand.write_joint_effort_limit(original_limits)
                print("  原始值恢复成功")
            except Exception as e:
                print(f"  [WARNING] 恢复原始值失败: {e}")

    print("\n" + "=" * 60)
    print(f"验证结果: {'PASS' if is_success else 'FAIL'}")
    print("=" * 60)

    return is_success


if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)
