try:
    from .hand_zenoh_bridge import (
        HandBridge,
        RESOURCE_DEFS,
        build_capability,
        get_timestamp_us,
        sanitize_sn,
        wrap_with_timestamp,
    )
    from .tactile_bridge import TactileBridge
except ModuleNotFoundError as e:
    if e.name != "zenoh":
        raise
    raise ImportError(
        "Zenoh bridge requires extra dependencies. "
        "Install with: pip install wujihandpy[bridge]"
    ) from e

__all__ = [
    "HandBridge",
    "TactileBridge",
    "RESOURCE_DEFS",
    "build_capability",
    "get_timestamp_us",
    "sanitize_sn",
    "wrap_with_timestamp",
]
