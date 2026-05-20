import wujihandpy

hand1 = wujihandpy.Hand(side="left")
hand2 = wujihandpy.Hand(side="right")

print("Input voltage(left): ", hand1.read_input_voltage())
print("Input voltage(right): ", hand2.read_input_voltage())
