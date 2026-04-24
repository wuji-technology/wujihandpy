"""Background firmware upgrade check for wujihandpy.

All logic here must never raise to the caller. Failures are logged at DEBUG
level via the `wujihandpy.upgrade_check` logger.
"""

from __future__ import annotations

import json
import logging
import os
import re
import socket
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


_log = logging.getLogger("wujihandpy.upgrade_check")


def decode_firmware_version(raw: int) -> str | None:
    """Decode a packed uint32 firmware version into a semver string.

    Byte layout (little-endian):
      byte 0: major
      byte 1: minor
      byte 2: patch
      byte 3: prerelease ASCII

    Prerelease byte convention (matches the wuji-hand-upgrader Tauri app):
      '~'   (0x7E) -> stable (no suffix)
      'A'..'Z' (0x41..0x5A) -> rc.0..rc.25
      anything else (including 0x00) -> invalid -> None
    """
    major = raw & 0xFF
    minor = (raw >> 8) & 0xFF
    patch = (raw >> 16) & 0xFF
    prerelease = (raw >> 24) & 0xFF

    if prerelease == 0x7E:  # '~' = stable release marker
        return f"{major}.{minor}.{patch}"
    if 0x41 <= prerelease <= 0x5A:  # 'A' .. 'Z' -> rc.0 .. rc.25
        return f"{major}.{minor}.{patch}-rc.{prerelease - 0x41}"
    return None


_VERSION_RE = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)(?:-rc\.(\d+))?$")


def parse_version(v: str) -> tuple[int, int, int, int, int] | None:
    """Parse a semver string into a comparable 5-tuple.

    Returns (major, minor, patch, stable_flag, rc_num):
      stable_flag = 1 for stable (no -rc), 0 for rc -> stable always ranks higher
      rc_num = 0 for stable, N for -rc.N

    Returns None for anything not matching X.Y.Z or X.Y.Z-rc.N (optional v prefix).
    """
    m = _VERSION_RE.match(v.strip())
    if not m:
        return None
    major, minor, patch = int(m[1]), int(m[2]), int(m[3])
    if m[4] is None:
        return (major, minor, patch, 1, 0)
    return (major, minor, patch, 0, int(m[4]))


def should_show_banner(current: str, latest: str) -> bool:
    """Return True iff latest is strictly newer than current and both parse."""
    cur = parse_version(current)
    lat = parse_version(latest)
    if cur is None or lat is None:
        return False
    return lat > cur


def slim_firmwares(raw_list: list[dict]) -> list[dict]:
    """Extract the fields we care about from raw FirmwareItem objects.

    The banner only renders the version diff — release notes were dropped
    in a later iteration — so each entry is just {version}. Drops entries
    without a manifest.version.
    """
    out: list[dict] = []
    for item in raw_list:
        manifest = item.get("manifest") or {}
        version = manifest.get("version")
        if not isinstance(version, str) or not version:
            continue
        out.append({"version": version})
    return out


def find_latest(firmwares: list[dict]) -> dict | None:
    """Return the firmware with the highest semver version, or None if empty/all-invalid."""
    parsed = []
    for f in firmwares:
        key = parse_version(f["version"])
        if key is not None:
            parsed.append((key, f))
    if not parsed:
        return None
    parsed.sort(key=lambda x: x[0], reverse=True)
    return parsed[0][1]


CACHE_TTL_SECONDS = 24 * 60 * 60


def _cache_path() -> Path:
    # Path.home() consults $HOME first, then falls back to pwd lookup —
    # mirrors the convention in wujihandcpp/src/logging/logging.hpp.
    return Path.home() / ".wuji" / "cache" / "wujihandpy" / "upgrade_check.json"


def _is_valid_cache(data: object) -> bool:
    if not isinstance(data, dict):
        return False
    if not isinstance(data.get("fetched_at"), (int, float)):
        return False
    if not isinstance(data.get("sn"), str) or not data["sn"]:
        return False
    if not isinstance(data.get("firmwares"), list):
        return False
    return True


def load_cache() -> dict | None:
    """Return the cached payload if fresh, else None.

    None means: file missing, JSON corrupted, schema unexpected, or TTL expired.
    """
    path = _cache_path()
    try:
        raw = path.read_text(encoding="utf-8")
    except (FileNotFoundError, OSError) as e:
        _log.debug("cache read failed: %s", e)
        return None
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as e:
        _log.debug("cache corrupted: %s", e)
        return None
    if not _is_valid_cache(data):
        _log.debug("cache schema mismatch")
        return None
    if time.time() - data["fetched_at"] >= CACHE_TTL_SECONDS:
        _log.debug("cache expired")
        return None
    return data


def save_cache(payload: dict) -> None:
    """Best-effort atomic cache write. Never raises.

    Writes to a sibling temp file then `os.replace`s into place so a process
    killed mid-write can't leave a truncated/corrupt cache file behind.
    """
    path = _cache_path()
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp_path = path.with_name(path.name + ".tmp")
        tmp_path.write_text(json.dumps(payload), encoding="utf-8")
        os.replace(tmp_path, path)
    except OSError as e:
        _log.debug("cache write failed: %s", e)


API_BASE_URL = "https://api.wuji.tech/v1/firmwares"
HTTP_TIMEOUT_SECONDS = 3.0


def _get_sdk_version() -> str:
    # Import the version submodule directly rather than the top-level
    # wujihandpy package to avoid any circular-import surprises (this
    # module is imported by wujihandpy/__init__.py while it's still
    # being initialized).
    try:
        from wujihandpy._version import __version__
        return __version__
    except Exception:
        return "unknown"


def fetch_firmwares(sn: str) -> list[dict] | None:
    """Return the slim firmware list for the device with the given SN.

    The wuji-admin open API resolves device_type by prefix-matching the
    given `sn` against `device_types.code`, then returns all published
    firmwares for that device type. Therefore we MUST pass the device's
    real SN (not the device-type code).

    Uses the 24h cache when the cached SN matches. Returns None on any
    network or API error. Never raises.
    """
    cache = load_cache()
    if cache is not None and cache.get("sn") == sn:
        return cache["firmwares"]

    url = f"{API_BASE_URL}?sn={urllib.parse.quote(sn)}"
    req = urllib.request.Request(
        url,
        headers={"User-Agent": f"wujihandpy/{_get_sdk_version()}"},
    )
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT_SECONDS) as resp:
            raw_bytes = resp.read()
    except (urllib.error.URLError, socket.timeout, OSError) as e:
        _log.debug("upgrade check HTTP failed: %s", e)
        return None

    try:
        payload = json.loads(raw_bytes)
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        _log.debug("upgrade check response parse failed: %s", e)
        return None

    if not isinstance(payload, dict) or payload.get("code") != 0:
        _log.debug("upgrade check API error: %s", payload if isinstance(payload, dict) else "non-dict")
        return None

    firmwares = slim_firmwares(payload.get("data") or [])
    save_cache({
        "fetched_at": int(time.time()),
        "sn": sn,
        "firmwares": firmwares,
    })
    return firmwares


UPGRADE_GUIDE_URL = (
    "https://docs.wuji.tech/docs/en/wuji-hand/latest/wuji-hand-upgrader-user-guide/"
)

# WUJIHAND pixel LOGO. Each letter is a 6-row glyph with proportional widths
# (W = 14 cols, I = 2 cols, others = 9-10 cols). Letters separated by 3-char
# gaps; total width = 96 chars. Glyph shapes mirror the reference design at
# /home/zenggui/windows_file/upgrade-notice.py exactly.
_LOGO_LINES = [
    "██          ██   ██      ██          ██   ██   ██      ██      ████      ██      ██   ██████    ",
    "██    ██    ██   ██      ██          ██   ██   ██      ██    ██    ██    ███     ██   ██    ██  ",
    " ██  ████  ██    ██      ██          ██   ██   ██████████   ██      ██   █████   ██   ██      ██",
    "  ██ ████ ██     ██      ██          ██   ██   ██      ██   ██████████   ██ ████ ██   ██      ██",
    "  ████  ████      ██    ██    ██     ██   ██   ██      ██   ██      ██   ██   █████   ██    ██  ",
    "   ██    ██        ██████      ███████    ██   ██      ██   ██      ██   ██     ███   ██████    ",
]
_LOGO_WIDTH = 96
_FRAME_WIDTH = _LOGO_WIDTH + 2     # ruler / separator overhang past LOGO edges
_BODY_INDENT = "  "                # 2-space indent on every printed line

_RULER = "━" * _FRAME_WIDTH        # heavy horizontal box-drawing for top/bottom
_SEPARATOR = "─" * _FRAME_WIDTH    # light horizontal for separator above URL

# 24-bit ANSI color helpers. Disabled when NO_COLOR env var is set.
_COLOR_RESET = "\033[0m"
_COLOR_BOLD = "\033[1m"
_COLOR_DIM = "\033[2m"
_COLOR_GRAY = "\033[90m"
_COLOR_GREEN = "\033[38;2;52;168;83m"     # google-style green for the new version
_COLOR_BLUE = "\033[38;2;43;127;255m"     # solid blue for the headline and URL
# Vertical blue gradient endpoints for the LOGO and rulers.
_GRADIENT_LIGHT = (120, 185, 255)
_GRADIENT_DARK = (22, 85, 215)


def _colors_enabled() -> bool:
    """ANSI color is on unless NO_COLOR is set (any non-empty value disables)."""
    return not os.environ.get("NO_COLOR")


def _gradient_blue(row: int, total_rows: int) -> str:
    """24-bit ANSI escape for a vertical blue gradient at the given row."""
    t = row / max(total_rows - 1, 1)
    r = round(_GRADIENT_LIGHT[0] + (_GRADIENT_DARK[0] - _GRADIENT_LIGHT[0]) * t)
    g = round(_GRADIENT_LIGHT[1] + (_GRADIENT_DARK[1] - _GRADIENT_LIGHT[1]) * t)
    b = round(_GRADIENT_LIGHT[2] + (_GRADIENT_DARK[2] - _GRADIENT_LIGHT[2]) * t)
    return f"\033[38;2;{r};{g};{b}m"


def _hyperlink(url: str, color: bool) -> str:
    """Wrap URL with OSC 8 hyperlink escape so modern terminals make it clickable."""
    if not color:
        return url
    return f"\033]8;;{url}\033\\{url}\033]8;;\033\\"


def _render_logo_block(color: bool) -> list[str]:
    """Top ruler + blank + 6 LOGO rows (vertical gradient) + blank + bottom ruler.

    Rulers are solid blue (matches the reference); gradient runs only across
    the 6 LOGO rows. Each line is indented to match the rest of the banner.
    """
    blue = _COLOR_BLUE if color else ""
    reset = _COLOR_RESET if color else ""

    out: list[str] = []
    out.append(f"{_BODY_INDENT}{blue}{_RULER}{reset}")
    out.append("")

    n = len(_LOGO_LINES)
    for i, row in enumerate(_LOGO_LINES):
        if not color:
            out.append(_BODY_INDENT + row)
            continue
        c = _gradient_blue(i, n)
        rendered = "".join((c + ch) if ch == "█" else ch for ch in row)
        out.append(_BODY_INDENT + rendered + _COLOR_RESET)

    out.append("")
    out.append(f"{_BODY_INDENT}{blue}{_RULER}{reset}")
    return out


def render_banner(current: str, latest_version: str) -> str:
    """Render the complete upgrade banner as a single string.

    Includes ANSI color (24-bit truecolor) and OSC 8 hyperlink for the URL
    when stderr is a TTY and NO_COLOR is unset. Self-contained: includes
    leading + trailing blank lines so a single sys.stderr.write() outputs
    a tidy block.
    """
    color = _colors_enabled()
    bold = _COLOR_BOLD if color else ""
    blue = _COLOR_BLUE if color else ""
    green = _COLOR_GREEN if color else ""
    dim = _COLOR_DIM if color else ""
    gray = _COLOR_GRAY if color else ""
    reset = _COLOR_RESET if color else ""

    lines: list[str] = [""]
    lines.extend(_render_logo_block(color))
    lines.append("")
    lines.append(
        f"{_BODY_INDENT}{blue}{bold}⬆ New firmware available{reset}"
        f"   v{current} {gray}→{reset} {green}{bold}v{latest_version}{reset}"
    )
    lines.append("")
    lines.append(f"{_BODY_INDENT}{gray}{_SEPARATOR}{reset}")
    lines.append("")
    lines.append(
        f"{_BODY_INDENT}{dim}Upgrade guide ›{reset}  "
        f"{blue}{_hyperlink(UPGRADE_GUIDE_URL, color)}{reset}"
    )
    lines.append("")

    return "\n".join(lines) + "\n"


def render_legacy_banner(latest_version: str | None) -> str:
    """Render the upgrade banner for legacy devices.

    Two flavors:
      - latest_version given (sn-known but local version unreadable):
        show "latest available: vX.Y.Z" so user knows the target.
      - latest_version is None (sn-unknown, can't query API at all):
        static "please upgrade" message with no version info.
    """
    color = _colors_enabled()
    bold = _COLOR_BOLD if color else ""
    blue = _COLOR_BLUE if color else ""
    green = _COLOR_GREEN if color else ""
    dim = _COLOR_DIM if color else ""
    gray = _COLOR_GRAY if color else ""
    reset = _COLOR_RESET if color else ""

    lines: list[str] = [""]
    lines.extend(_render_logo_block(color))
    lines.append("")

    if latest_version is not None:
        lines.append(
            f"{_BODY_INDENT}{blue}{bold}⬆ Firmware upgrade recommended{reset}"
            f"   latest available: {green}{bold}v{latest_version}{reset}"
        )
        lines.append("")
        lines.append(f"{_BODY_INDENT}{dim}Your device firmware is too old to report its system version.{reset}")
        lines.append(f"{_BODY_INDENT}{dim}Please follow the upgrade guide to update.{reset}")
    else:
        lines.append(f"{_BODY_INDENT}{blue}{bold}⬆ Firmware upgrade strongly recommended{reset}")
        lines.append("")
        lines.append(f"{_BODY_INDENT}{dim}The upgrade service can't identify your firmware—it's out of date.{reset}")
        lines.append(f"{_BODY_INDENT}{dim}Please follow the upgrade guide to update.{reset}")

    lines.append("")
    lines.append(f"{_BODY_INDENT}{gray}{_SEPARATOR}{reset}")
    lines.append("")
    lines.append(
        f"{_BODY_INDENT}{dim}Upgrade guide ›{reset}  "
        f"{blue}{_hyperlink(UPGRADE_GUIDE_URL, color)}{reset}"
    )
    lines.append("")

    return "\n".join(lines) + "\n"


_checked_sns: set[str] = set()
_lock = threading.Lock()


_LEGACY_NO_SN_KEY = "__legacy_no_sn__"


def _run_check_sync(sn: str, raw_version: int | None) -> None:
    """Synchronous core of the upgrade check.

    Called by the background worker thread with pure values read on the
    Hand's construction thread. Never raises.

    Three branches based on what we read:
      1. sn empty       -> static "firmware too old" banner, skip API call
                           (firmware predates SN reporting; API needs SN)
      2. sn + bad ver   -> fetch API, render legacy banner with latest version
                           but no version diff
      3. sn + good ver  -> normal banner with version diff if newer exists
    """
    try:
        if not isinstance(sn, str):
            return

        # Pre-check dedup: if we already printed for this SN (or for the
        # legacy-no-SN case) in this process, bail out without doing any
        # I/O. We commit the dedup AFTER a successful banner write so
        # transient failures (e.g., temporary network outage) don't burn
        # the SN for the rest of the process.
        dedup_key = sn if sn else _LEGACY_NO_SN_KEY
        with _lock:
            if dedup_key in _checked_sns:
                return

        # Branch 1: very old firmware (no SN) -> static legacy banner, no API
        if not sn:
            _log.debug("upgrade check: empty SN, rendering static legacy banner")
            banner = render_legacy_banner(latest_version=None)
            _emit_banner(banner, dedup_key)
            return

        # 2) Decode current version (may be None for legacy devices)
        current: str | None = None
        if raw_version is not None:
            decoded = decode_firmware_version(raw_version)
            if decoded is not None and not decoded.startswith("0.0.0"):
                current = decoded
            else:
                _log.debug(
                    "upgrade check: current version unreadable, raw=0x%08x decoded=%r",
                    raw_version, decoded,
                )

        # 3) Fetch (per-device SN; API resolves device_type by prefix match)
        firmwares = fetch_firmwares(sn)
        if not firmwares:
            return

        latest = find_latest(firmwares)
        if latest is None:
            return

        # 4) Render + write
        if current is not None:
            if not should_show_banner(current, latest["version"]):
                return
            banner = render_banner(
                current=current,
                latest_version=latest["version"],
            )
        else:
            # Legacy device: current version unknown -> generic prompt
            banner = render_legacy_banner(latest_version=latest["version"])

        _emit_banner(banner, dedup_key)
    except Exception as e:  # 安全网
        _log.debug("upgrade check: unexpected error: %s", e)


def _emit_banner(banner: str, dedup_key: str) -> None:
    """Write the banner to stderr and, only on success, commit the dedup key.

    Splitting write + dedup keeps a transient stderr failure from suppressing
    future banners for the same key in this process.
    """
    try:
        sys.stderr.write(banner)
        sys.stderr.flush()
    except Exception as e:
        _log.debug("upgrade check: stderr write failed: %s", e)
        return
    with _lock:
        _checked_sns.add(dedup_key)


def trigger_check_in_background(sn: str, raw_version: int | None) -> None:
    """Fire-and-forget upgrade check. Returns immediately.

    No-op if stderr is not a TTY — saves a thread + an HTTP round trip.
    Caller must have already read `sn` (and optionally `raw_version`) on
    the Hand's construction thread to comply with the C++ thread-safety
    contract. Pass raw_version=None for legacy devices to render the
    generic "version unknown" banner.
    """
    try:
        if not sys.stderr.isatty():
            return
    except Exception:
        return  # 极少见情况: stderr 是 None 或被替换

    t = threading.Thread(
        target=_run_check_sync,
        args=(sn, raw_version),
        name="wujihandpy-upgrade-check",
        daemon=True,
    )
    t.start()
