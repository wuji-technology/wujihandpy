import wujihandpy
import numpy as np
import math
import asyncio


async def main():
    hand = wujihandpy.Hand()
    try:
        # Read and print motor temperatures per second while shaking hand
        await asyncio.gather(shake(hand), read_temperature(hand))
    finally:
        # Disable the entire hand
        await hand.write_joint_enabled_async(False)


async def shake(hand: wujihandpy.Hand):
    # Enable all joints
    await hand.write_joint_enabled_async(True)

    # Filtered realtime control (100Hz -> 16kHz)
    controller = hand.realtime_controller(
        enable_upstream=False, filter=wujihandpy.filter.LowPass(cutoff_freq=2.0)
    )
    update_rate = 100.0
    update_period = 1.0 / update_rate

    x = 0
    while True:
        y = (1 - math.cos(x)) * 0.8

        controller.set_joint_target_position(
            np.array(
                [
                    # J1J2 J3J4
                    [0, 0, 0, 0],  # F1
                    [y, 0, y, y],  # F2
                    [y, 0, y, y],  # F3
                    [y, 0, y, y],  # F4
                    [y, 0, y, y],  # F5
                ],
                dtype=np.float64,
            )
        )

        x += math.pi / update_rate
        await asyncio.sleep(update_period)


async def read_temperature(hand: wujihandpy.Hand):
    while True:
        print("Motor temperatures: \n", await hand.read_joint_temperature_async())
        await asyncio.sleep(1)


if __name__ == "__main__":
    asyncio.run(main())
