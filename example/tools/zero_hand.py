import wujihandpy
import numpy as np
import sys
import time

def main():
    try:
        hand = wujihandpy.Hand()
        
        print("确保灵巧手已使能...")
        hand.write_joint_enabled(True)
        
        # 稍作等待以确保使能生效
        time.sleep(0.5)
        
        print("正在回归 0 位...")
        # 创建 5x4 的全 0 矩阵 (5个手指 x 4个关节)
        zero_positions = np.zeros((5, 4), dtype=np.float64)
        
        # 写入目标位置
        hand.write_joint_target_position(zero_positions)
        print("回归 0 位指令已发送。")
        
    except Exception as e:
        print(f"操作失败: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
