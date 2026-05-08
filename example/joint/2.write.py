import wujihandpy
import numpy as np

hand = wujihandpy.Hand()

# Bulk-Write: Enable index finger and disable other fingers
hand.write_joint_enabled(
    np.array(
        [
            #  J1     J2     J3     J4
            [False, False, False, False],  # F1
            [ True,  True,  True,  True],  # F2
            [False, False, False, False],  # F3
            [False, False, False, False],  # F4
            [False, False, False, False],  # F5
        ],
        dtype=bool,
    )
)
print("Successfully enable index finger, disabled other fingers.")

# Bulk-Write: Now enable them all
hand.write_joint_enabled(True)
print("Successfully enabled all.")

# Enable all one by one
for i in range(5):
    for j in range(4):
        hand.finger(i).joint(j).write_joint_enabled(True)
print("Successfully enabled all one by one.")
