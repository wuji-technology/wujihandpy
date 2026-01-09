import wujihandpy

hand = wujihandpy.Hand()

# Read
print("Input voltage: ", hand.read_input_voltage())

# Bulk-Read: Return 5x4 np.array
print("Motor temperatures: \n", hand.read_joint_temperature())

# Read effort limit (returns 5x4 array, default 1.5A)
print("Effort limit: \n", hand.read_joint_effort_limit())

# Normal APIs are blocking to ensure successful operations