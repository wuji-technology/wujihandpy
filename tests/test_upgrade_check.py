"""Tests for wujihandpy._upgrade_check."""

from wujihandpy._upgrade_check import decode_firmware_version


def test_decode_stable_tilde():
    # '~' (0x7E) = stable release marker; major=1, minor=2, patch=0
    assert decode_firmware_version(0x7E000201) == "1.2.0"


def test_decode_stable_another():
    # '~' marker, major=3, minor=3, patch=0
    assert decode_firmware_version(0x7E000303) == "3.3.0"


def test_decode_rc_zero():
    # 'A' (0x41) = rc.0, version 2.3.3
    assert decode_firmware_version(0x41030302) == "2.3.3-rc.0"


def test_decode_rc_max():
    # 'Z' (0x5A) = rc.25
    assert decode_firmware_version(0x5A030302) == "2.3.3-rc.25"


def test_decode_invalid_zero_byte():
    # 0x00 prerelease byte = invalid (matches upgrader convention; only ~ means stable)
    assert decode_firmware_version(0x00000201) is None


def test_decode_invalid_prerelease_lowercase():
    # 'a' = 0x61, not uppercase A-Z and not '~' -> invalid
    assert decode_firmware_version(0x61030302) is None


def test_decode_invalid_prerelease_symbol():
    # '?' = 0x3F, not a letter and not '~' -> invalid
    assert decode_firmware_version(0x3F030302) is None


from wujihandpy._upgrade_check import parse_version, should_show_banner


def test_parse_stable_basic():
    assert parse_version("3.3.0") == (3, 3, 0, 1, 0)


def test_parse_v_prefix():
    assert parse_version("v3.3.0") == parse_version("3.3.0")


def test_parse_rc():
    assert parse_version("3.3.0-rc.5") == (3, 3, 0, 0, 5)


def test_parse_invalid():
    assert parse_version("garbage") is None
    assert parse_version("3.3") is None
    assert parse_version("3.3.0.0") is None
    assert parse_version("3.3.0-beta") is None


def test_compare_patch():
    assert parse_version("3.3.0") > parse_version("3.2.9")


def test_compare_stable_beats_rc():
    assert parse_version("3.3.0") > parse_version("3.3.0-rc.5")


def test_compare_rc_order():
    assert parse_version("3.3.0-rc.2") > parse_version("3.3.0-rc.1")


def test_show_banner_when_newer():
    assert should_show_banner("1.2.0", "3.3.0") is True


def test_no_banner_when_equal():
    assert should_show_banner("3.3.0", "3.3.0") is False


def test_no_banner_when_current_newer():
    # 开发 / 回滚场景，不吓用户
    assert should_show_banner("3.3.1", "3.3.0") is False


def test_no_banner_when_unparseable():
    assert should_show_banner("garbage", "3.3.0") is False
    assert should_show_banner("3.3.0", "garbage") is False


from wujihandpy._upgrade_check import find_latest, slim_firmwares


def test_find_latest_picks_highest():
    firmwares = [
        {"version": "3.2.0", "release_notes_en": [], "release_notes_zh": []},
        {"version": "3.3.0", "release_notes_en": ["x"], "release_notes_zh": []},
        {"version": "3.1.5", "release_notes_en": [], "release_notes_zh": []},
    ]
    assert find_latest(firmwares)["version"] == "3.3.0"


def test_find_latest_stable_beats_rc():
    firmwares = [
        {"version": "3.3.0-rc.2", "release_notes_en": [], "release_notes_zh": []},
        {"version": "3.3.0", "release_notes_en": [], "release_notes_zh": []},
    ]
    assert find_latest(firmwares)["version"] == "3.3.0"


def test_find_latest_empty():
    assert find_latest([]) is None


def test_find_latest_all_unparseable():
    firmwares = [{"version": "garbage", "release_notes_en": [], "release_notes_zh": []}]
    assert find_latest(firmwares) is None


def test_slim_firmwares_extracts_only_version():
    # 模拟 wuji-admin 返回的 FirmwareItem 原文
    raw = [
        {
            "id": "uuid-1",
            "device_type_code": "hand",
            "version": "3.3.0",
            "file_size": 1234,
            "sha256": "abc",
            "download_url": "/v1/firmwares/uuid-1/download",
            "manifest": {
                "version": "3.3.0",
                "digest": {"algorithm": "sha256", "value": "abc"},
                "release_notes": {
                    "zh-CN": ["中文1", "中文2"],
                    "en-US": ["English 1"],
                },
            },
        },
    ]
    slim = slim_firmwares(raw)
    # Only `version` is kept; release notes and other fields are discarded.
    assert slim == [{"version": "3.3.0"}]


def test_slim_firmwares_minimal_manifest():
    raw = [{"manifest": {"version": "3.3.0"}}]
    slim = slim_firmwares(raw)
    assert slim == [{"version": "3.3.0"}]


def test_slim_firmwares_skips_invalid_entries():
    # manifest 缺失或没有 version 的条目直接丢弃
    raw = [
        {"manifest": {"version": "3.3.0"}},
        {"manifest": {}},
        {},
    ]
    slim = slim_firmwares(raw)
    assert len(slim) == 1


import json
import time
from pathlib import Path

from wujihandpy._upgrade_check import (
    CACHE_TTL_SECONDS,
    _cache_path,
    load_cache,
    save_cache,
)


def test_cache_path_is_under_wuji_cache(monkeypatch, tmp_path):
    monkeypatch.setenv("HOME", str(tmp_path))
    p = _cache_path()
    assert p.name == "upgrade_check.json"
    assert p.parent.name == "wujihandpy"
    assert p.parent.parent.name == "cache"
    assert p.parent.parent.parent.name == ".wuji"


def test_save_and_load_roundtrip(monkeypatch, tmp_path):
    monkeypatch.setenv("HOME", str(tmp_path))
    payload = {
        "fetched_at": int(time.time()),
        "sn": "TEST_SN",
        "firmwares": [{"version": "3.3.0", "release_notes_en": [], "release_notes_zh": []}],
    }
    save_cache(payload)
    loaded = load_cache()
    assert loaded == payload


def test_load_cache_missing_file_returns_none(monkeypatch, tmp_path):
    monkeypatch.setenv("HOME", str(tmp_path))
    assert load_cache() is None


def test_load_cache_corrupted_json_returns_none(monkeypatch, tmp_path):
    monkeypatch.setenv("HOME", str(tmp_path))
    path = _cache_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("this is not json")
    assert load_cache() is None


def test_load_cache_expired_returns_none(monkeypatch, tmp_path):
    monkeypatch.setenv("HOME", str(tmp_path))
    old_payload = {
        "fetched_at": int(time.time()) - CACHE_TTL_SECONDS - 1,
        "sn": "TEST_SN",
        "firmwares": [],
    }
    save_cache(old_payload)
    assert load_cache() is None


def test_load_cache_wrong_schema_returns_none(monkeypatch, tmp_path):
    monkeypatch.setenv("HOME", str(tmp_path))
    path = _cache_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"unexpected": "schema"}))
    assert load_cache() is None


def test_save_cache_creates_parent_dirs(monkeypatch, tmp_path):
    monkeypatch.setenv("HOME", str(tmp_path))
    assert not (tmp_path / ".wuji").exists()
    save_cache({"fetched_at": 0, "sn": "TEST_SN", "firmwares": []})
    assert _cache_path().exists()


def test_save_cache_swallows_write_errors(monkeypatch, tmp_path):
    # 父目录被替换成文件,无法写入
    monkeypatch.setenv("HOME", str(tmp_path))
    (tmp_path / ".wuji").write_text("blocking file")
    # 不应该抛异常
    save_cache({"fetched_at": 0, "sn": "TEST_SN", "firmwares": []})


import io
import socket
from unittest.mock import patch

from wujihandpy._upgrade_check import fetch_firmwares


def _fake_http_response(code: int, msg: str = "", data: list | None = None) -> io.BytesIO:
    """Build a BytesIO that mimics urlopen's return value (BytesIO already supports `with`)."""
    body = json.dumps({"code": code, "msg": msg, "data": data or []}).encode()
    return io.BytesIO(body)


def test_fetch_uses_fresh_cache(monkeypatch, tmp_path):
    monkeypatch.setenv("HOME", str(tmp_path))
    save_cache({
        "fetched_at": int(time.time()),
        "sn": "TEST_SN",
        "firmwares": [{"version": "3.3.0", "release_notes_en": [], "release_notes_zh": []}],
    })
    # 不应该发起 HTTP 请求
    with patch("wujihandpy._upgrade_check.urllib.request.urlopen") as mock_open:
        result = fetch_firmwares("TEST_SN")
    assert mock_open.called is False
    assert result[0]["version"] == "3.3.0"


def test_fetch_http_success_writes_cache(monkeypatch, tmp_path):
    monkeypatch.setenv("HOME", str(tmp_path))
    api_payload = [{
        "manifest": {
            "version": "3.3.0",
            "release_notes": {"en-US": ["note"], "zh-CN": ["中文"]},
        },
    }]
    with patch(
        "wujihandpy._upgrade_check.urllib.request.urlopen",
        return_value=_fake_http_response(0, "", api_payload),
    ):
        result = fetch_firmwares("TEST_SN")
    # Release notes were dropped in slim_firmwares; only version is kept.
    assert result == [{"version": "3.3.0"}]
    # 缓存写到了磁盘
    assert _cache_path().exists()


def test_fetch_http_timeout_returns_none(monkeypatch, tmp_path):
    monkeypatch.setenv("HOME", str(tmp_path))
    with patch(
        "wujihandpy._upgrade_check.urllib.request.urlopen",
        side_effect=socket.timeout("timed out"),
    ):
        assert fetch_firmwares("TEST_SN") is None


def test_fetch_http_url_error_returns_none(monkeypatch, tmp_path):
    import urllib.error
    monkeypatch.setenv("HOME", str(tmp_path))
    with patch(
        "wujihandpy._upgrade_check.urllib.request.urlopen",
        side_effect=urllib.error.URLError("dns fail"),
    ):
        assert fetch_firmwares("TEST_SN") is None


def test_fetch_api_error_code_returns_none(monkeypatch, tmp_path):
    monkeypatch.setenv("HOME", str(tmp_path))
    with patch(
        "wujihandpy._upgrade_check.urllib.request.urlopen",
        return_value=_fake_http_response(500, "internal error"),
    ):
        assert fetch_firmwares("TEST_SN") is None


def test_fetch_invalid_json_returns_none(monkeypatch, tmp_path):
    monkeypatch.setenv("HOME", str(tmp_path))
    bad = io.BytesIO(b"not json")
    with patch("wujihandpy._upgrade_check.urllib.request.urlopen", return_value=bad):
        assert fetch_firmwares("TEST_SN") is None

from wujihandpy._upgrade_check import render_banner, UPGRADE_GUIDE_URL


def test_banner_contains_versions():
    banner = render_banner(current="1.2.0", latest_version="3.3.0")
    assert "1.2.0" in banner
    assert "3.3.0" in banner
    assert "New firmware available" in banner


def test_banner_contains_logo_block_char():
    banner = render_banner("1.2.0", "3.3.0")
    assert "█" in banner  # LOGO 是块字符
    assert "═" in banner  # 顶底 ruler


def test_banner_contains_upgrade_url():
    banner = render_banner("1.2.0", "3.3.0")
    assert UPGRADE_GUIDE_URL in banner
    assert UPGRADE_GUIDE_URL.startswith("https://docs.wuji.tech/")


def test_banner_has_no_bullet_section():
    """Release notes were removed; banner should not contain bullets or notes lines."""
    banner = render_banner("1.2.0", "3.3.0")
    assert "•" not in banner
    assert "…" not in banner


def test_banner_includes_color_by_default(monkeypatch):
    monkeypatch.delenv("NO_COLOR", raising=False)
    banner = render_banner("1.2.0", "3.3.0")
    assert "\033[" in banner   # at least one ANSI escape
    assert "\033[0m" in banner  # and a reset


def test_banner_omits_color_when_no_color_env_set(monkeypatch):
    monkeypatch.setenv("NO_COLOR", "1")
    banner = render_banner("1.2.0", "3.3.0")
    assert "\033[" not in banner


from wujihandpy._upgrade_check import render_legacy_banner


def test_legacy_banner_has_latest_version():
    banner = render_legacy_banner("3.3.0")
    assert "3.3.0" in banner
    assert "Firmware upgrade recommended" in banner
    assert UPGRADE_GUIDE_URL in banner
    # 仍然有 LOGO 和 ruler
    assert "█" in banner
    assert "═" in banner


def test_legacy_banner_omits_release_notes():
    """Legacy banner should NOT show release notes (no current version to diff from)."""
    banner = render_legacy_banner("3.3.0")
    assert "•" not in banner


def test_legacy_banner_no_arrow_diff():
    """Legacy banner shouldn't show 'vX -> vY' since current is unknown."""
    banner = render_legacy_banner("3.3.0")
    assert "→" not in banner


def test_legacy_banner_static_when_latest_is_none():
    """latest_version=None -> static banner with no version info but full LOGO/URL."""
    banner = render_legacy_banner(None)
    assert "strongly recommended" in banner
    # Crucially: no specific latest-version string
    assert "latest available" not in banner
    # But shell is intact
    assert "█" in banner
    assert "═" in banner
    assert UPGRADE_GUIDE_URL in banner


from wujihandpy._upgrade_check import (
    _run_check_sync,
    _checked_sns,
    _lock,
    trigger_check_in_background,
)


def _reset_sn_dedup():
    with _lock:
        _checked_sns.clear()


def _stub_fetch(monkeypatch, firmwares):
    """Mock fetch_firmwares so worker tests don't need real HTTP or cache."""
    monkeypatch.setattr(
        "wujihandpy._upgrade_check.fetch_firmwares",
        lambda sn: firmwares,
    )


def test_worker_prints_banner_on_new_version(monkeypatch, capsys):
    _reset_sn_dedup()
    _stub_fetch(monkeypatch, [
        {"version": "3.3.0", "release_notes_en": ["note"], "release_notes_zh": []},
    ])
    _run_check_sync("SN_TEST_001", 0x7E000201)  # 1.2.0
    err = capsys.readouterr().err
    assert "1.2.0" in err
    assert "3.3.0" in err
    assert "New firmware available" in err


def test_worker_silent_when_up_to_date(monkeypatch, capsys):
    _reset_sn_dedup()
    _stub_fetch(monkeypatch, [
        {"version": "1.2.0", "release_notes_en": [], "release_notes_zh": []},
    ])
    _run_check_sync("SN_TEST_002", 0x7E000201)
    assert capsys.readouterr().err == ""


def test_worker_dedups_same_sn(monkeypatch, capsys):
    _reset_sn_dedup()
    _stub_fetch(monkeypatch, [
        {"version": "3.3.0", "release_notes_en": [], "release_notes_zh": []},
    ])
    _run_check_sync("SN_DEDUP", 0x7E000201)
    _run_check_sync("SN_DEDUP", 0x7E000201)  # 二次调用不应再打
    err = capsys.readouterr().err
    assert err.count("New firmware available") == 1


def test_worker_different_sns_both_print(monkeypatch, capsys):
    _reset_sn_dedup()
    _stub_fetch(monkeypatch, [
        {"version": "3.3.0", "release_notes_en": [], "release_notes_zh": []},
    ])
    _run_check_sync("SN_A", 0x7E000201)
    _run_check_sync("SN_B", 0x7E000201)
    err = capsys.readouterr().err
    assert err.count("New firmware available") == 2


def test_worker_legacy_banner_when_raw_version_is_none(monkeypatch, capsys):
    """raw_version=None (legacy device, version read failed) -> legacy banner."""
    _reset_sn_dedup()
    _stub_fetch(monkeypatch, [
        {"version": "3.3.0", "release_notes_en": ["x"], "release_notes_zh": []},
    ])
    _run_check_sync("SN_LEGACY_NONE", None)
    err = capsys.readouterr().err
    assert "Firmware upgrade recommended" in err
    assert "3.3.0" in err
    # Legacy banner skips release notes
    assert "x" not in err.split("Upgrade guide")[0]


def test_worker_legacy_banner_when_decode_invalid(monkeypatch, capsys):
    """raw_version decodes to None (illegal prerelease byte) -> legacy banner."""
    _reset_sn_dedup()
    _stub_fetch(monkeypatch, [
        {"version": "3.3.0", "release_notes_en": [], "release_notes_zh": []},
    ])
    # Byte 3 = 0x61 ('a', lowercase) -> decode returns None -> legacy path
    _run_check_sync("SN_LEGACY_BAD", 0x61030302)
    err = capsys.readouterr().err
    assert "Firmware upgrade recommended" in err
    assert "3.3.0" in err


def test_worker_legacy_banner_when_decode_zero(monkeypatch, capsys):
    """raw_version that decodes to 0.0.0 -> legacy banner."""
    _reset_sn_dedup()
    _stub_fetch(monkeypatch, [
        {"version": "3.3.0", "release_notes_en": [], "release_notes_zh": []},
    ])
    # 0x7E000000 -> 0.0.0 stable -> still considered "unreadable"
    _run_check_sync("SN_LEGACY_ZERO", 0x7E000000)
    err = capsys.readouterr().err
    assert "Firmware upgrade recommended" in err


def test_worker_empty_sn_renders_static_legacy_banner(monkeypatch, capsys):
    """Empty SN (very old firmware) -> static legacy banner, no API call."""
    _reset_sn_dedup()

    def fail_fetch(_):
        raise AssertionError("fetch should not be called when SN is empty")

    monkeypatch.setattr("wujihandpy._upgrade_check.fetch_firmwares", fail_fetch)
    _run_check_sync("", 0x7E000001)
    err = capsys.readouterr().err
    assert "strongly recommended" in err
    assert "█" in err  # LOGO present
    assert "https://docs.wuji.tech" in err
    assert "latest available" not in err  # no version info


def test_worker_empty_sn_dedups_across_calls(monkeypatch, capsys):
    """Empty SN should print legacy banner only once per process."""
    _reset_sn_dedup()

    def fail_fetch(_):
        raise AssertionError("fetch should not be called")

    monkeypatch.setattr("wujihandpy._upgrade_check.fetch_firmwares", fail_fetch)
    _run_check_sync("", None)
    _run_check_sync("", None)
    _run_check_sync("", 0x7E000001)
    err = capsys.readouterr().err
    assert err.count("strongly recommended") == 1


def test_worker_silent_when_fetch_returns_none(monkeypatch, tmp_path, capsys):
    _reset_sn_dedup()
    monkeypatch.setenv("HOME", str(tmp_path))
    # 没有缓存 + HTTP 失败
    with patch(
        "wujihandpy._upgrade_check.urllib.request.urlopen",
        side_effect=socket.timeout("x"),
    ):
        _run_check_sync("SN_OFFLINE", 0x7E000201)
    assert capsys.readouterr().err == ""


def test_trigger_skips_when_not_a_tty(monkeypatch, capsys):
    _reset_sn_dedup()
    # 默认 pytest capsys 下 stderr 已经不是 TTY,但保险起见显式 mock
    monkeypatch.setattr("sys.stderr.isatty", lambda: False)
    trigger_check_in_background("SN_NOTTY", 0x7E000201)
    # 线程不会发起,stderr 上什么也没有
    time.sleep(0.1)  # 给线程一点点时间
    assert capsys.readouterr().err == ""


def test_trigger_uncaught_exception_never_escapes(monkeypatch, capsys):
    """A non-int raw_version triggers TypeError inside decode -> safety net swallows it."""
    _reset_sn_dedup()
    monkeypatch.setattr("sys.stderr.isatty", lambda: True)
    # Pass a non-int raw_version -> decode_firmware_version will raise TypeError.
    # The outer try/except in _run_check_sync must catch it.
    trigger_check_in_background("SN_BROKEN", "not an int")  # type: ignore[arg-type]
    # Main thread did not raise (we reached here). Wait for background thread
    # to finish so any crash would be visible before test ends.
    time.sleep(0.2)
