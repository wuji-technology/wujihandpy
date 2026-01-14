"""
write_joint_current_limit 兼容性验证脚本

前置条件：
- Python SDK 已安装
- 设备已连接

用例步骤与预期结果：
1. 调用 write_joint_current_limit() 方法
2. 预期：方法仍可正常使用，功能与 effort_limit 一致
"""

import sys
import wujihandpy
import numpy as np
import time
import warnings

def main():
    print("=" * 60)
    print("write_joint_current_limit 兼容性验证")
    print("=" * 60)

    # 启用所有警告显示，确保能看到 DeprecationWarning
    warnings.simplefilter('always')

    # 连接设备
    print("\n[步骤 1] 连接设备...")
    try:
        hand = wujihandpy.Hand()
        print("  设备连接成功")
    except Exception as e:
        print(f"  [ERROR] 连接设备失败: {e}")
        return False

    try:
        # 1. 读取当前 Current Limit (备份)
        print("\n[步骤 2] 调用 read_joint_current_limit() 读取当前值 (作为备份)...")
        # 使用旧接口读取
        original_limits = hand.read_joint_current_limit()
        print(f"  当前 Current Limit (前 5 个数据):\n  {original_limits.flatten()[:5]} ...")

        # 2. 设置新的 Current Limit
        # 设定一个安全的测试值，例如 0.8A (800mA)
        # 使用与 8.write_effort_limit_verify.py 不同的值以便区分
        test_limit_value = 0.8
        test_limits = np.full((5, 4), test_limit_value, dtype=np.float64)

        print(f"\n[步骤 3] 调用 write_joint_current_limit() 设置为全是 {test_limit_value} A ...")
        # 使用旧接口写入
        hand.write_joint_current_limit(test_limits)
        print("  写入命令执行完成")
        
        # 稍作等待
        time.sleep(0.1)

        # 3. 再次读取并验证
        print("\n[步骤 4] 读取并验证设置是否生效 (交叉验证)...")
        
        # 使用新接口验证，确保两者操作的是同一个底层数据
        print("  使用 read_joint_effort_limit() (新接口) 验证:")
        current_effort_limits = hand.read_joint_effort_limit()
        print(f"  读取到的 Effort Limit: \n  {current_effort_limits.flatten()[:5]} ...")
        
        # 使用旧接口验证
        print("  使用 read_joint_current_limit() (旧接口) 验证:")
        current_current_limits = hand.read_joint_current_limit()
        print(f"  读取到的 Current Limit:\n  {current_current_limits.flatten()[:5]} ...")

        # 验证差异
        diff = np.abs(current_current_limits - test_limits)
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
            print("\n[步骤 5] 恢复原始值...")
            try:
                # 尽量使用新接口恢复，保证状态正确
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
