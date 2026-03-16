"""Wuji Hand Zenoh Bridge - exposes wujihandpy via Zenoh for wuji-sdk."""

import json
import time
import threading
import logging
import argparse

import zenoh
import numpy as np

logger = logging.getLogger("hand_bridge")


def main():
    parser = argparse.ArgumentParser(description="Wuji Hand Zenoh Bridge")
    parser.add_argument("--sn", type=str, default=None, help="Hand serial number")
    parser.add_argument("--pub-rate", type=float, default=50.0, help="Position publish rate (Hz)")
    parser.add_argument("--log-level", type=str, default="INFO", help="Log level")
    args = parser.parse_args()

    logging.basicConfig(level=getattr(logging, args.log_level.upper()))
    logger.info("Hand Zenoh Bridge starting...")


if __name__ == "__main__":
    main()
