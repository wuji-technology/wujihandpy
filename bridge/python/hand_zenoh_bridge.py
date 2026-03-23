"""Compatibility wrapper for the packaged Python Zenoh Bridge."""

from wujihandpy.bridge.hand_zenoh_bridge import *  # noqa: F403
from wujihandpy.bridge.hand_zenoh_bridge import main


if __name__ == "__main__":
    main()
