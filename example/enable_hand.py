import wujihandpy
import sys

def main():
    try:
        hand = wujihandpy.Hand()
        print("正在连接并使能灵巧手...")
        hand.write_joint_enabled(True)
        print("所有关节已使能。")
    except Exception as e:
        print(f"连接或使能失败: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
