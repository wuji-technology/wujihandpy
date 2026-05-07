"""USB disconnect handling. Run this script and unplug the cable mid-read."""
import wujihandpy
import time


def main():
    hand = wujihandpy.Hand()
    print("Reading. Unplug USB to test disconnect handling.\n")

    try:
        while True:
            v = hand.read_input_voltage()
            print(f"input_voltage: {v:.2f}V")
            time.sleep(0.5)
    except ConnectionError as e:
        print(f"\nDisconnect detected: {e}")


if __name__ == "__main__":
    main()
