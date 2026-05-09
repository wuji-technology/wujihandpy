import threading
import time
import wujihandpy
from wujihandpy import Hand

# Create mutex lock to ensure thread safety
lock = threading.Lock()

def reader_thread(ctrl):
    """Read joint position periodically"""
    update_rate = 100.0
    update_period = 1.0 / update_rate

    while True:
        with lock:
            position = ctrl.get_joint_actual_position()
        print(f"[Reader] Position:\n{position}")
        time.sleep(update_period)

def writer_thread(ctrl):
    """Write target position periodically using cosine wave (same as realtime.py)"""
    import numpy as np
    import math

    update_rate = 100.0
    update_period = 1.0 / update_rate
    x = 0

    while True:
        y = (1 - math.cos(x)) * 0.8
        target = np.array(
            [
                [0, 0, 0, 0],  # F1
                [y, 0, y, y],  # F2
                [y, 0, y, y],  # F3
                [y, 0, y, y],  # F4
                [y, 0, y, y],  # F5
            ],
            dtype=np.float64,
        )
        with lock:
            ctrl.set_joint_target_position(target)
        print(f"[Writer] Target: {y:.4f} rad")
        x += math.pi / update_rate
        time.sleep(update_period)

def main():

    hand = Hand()

    # Disable thread safety check to allow multi-threaded operations
    hand.disable_thread_safe_check()
    print("Thread safety check disabled, multi-threaded access enabled")

    # Enable all joints first
    hand.write_joint_enabled(True)

    with hand.realtime_controller(
        enable_upstream=True, filter=wujihandpy.filter.LowPass(cutoff_freq=5.0)
    ) as ctrl:

        # Create reader and writer threads
        threads = [
            threading.Thread(target=reader_thread, args=(ctrl,)),
            threading.Thread(target=writer_thread, args=(ctrl,)),
        ]

        # Start all threads
        for t in threads:
            t.start()

        # Wait for threads
        for t in threads:
            t.join()

if __name__ == "__main__":
    main()
