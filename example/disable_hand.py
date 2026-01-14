import wujihandpy
import sys

def main():
    try:
        hand = wujihandpy.Hand()
        print("正在失能灵巧手...")
        hand.write_joint_enabled(False)
        print("所有关节已失能。")
    except Exception as e:
        print(f"操作失败: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
