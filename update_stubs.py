"""Regenerate native pybind stubs."""

import re

import pybind11_stubgen


def main():
    pybind11_stubgen.main(["wujihandpy._core", "-o", "src"])
    post_process_async_function()
    post_process_tactile_platform_guard()
    post_process_tactile_callbacks()


def post_process_async_function():
    with open("src/wujihandpy/_core/__init__.pyi", "r", encoding="utf-8") as file:
        content = file.read()

    sections = content.split("class")
    for i in range(len(sections)):
        return_type_dict: dict[str, str] = {}
        matches = re.finditer(r"def (read|write)_(\S+?)\(.+?\) -> ([^:]+):", sections[i])
        for match in matches:
            groups = match.groups()
            if not groups[1].endswith("async") and not groups[1].endswith("unchecked"):
                return_type_dict[f"{groups[0]}_{groups[1]}"] = groups[2].strip()

        for key, value in return_type_dict.items():
            sections[i] = re.sub(
                rf"def {key}_async\((.+?)\) -> typing\.Any:",
                lambda match: f"def {key}_async({match.group(1)}) -> typing.Awaitable[{value}]:",
                sections[i],
            )

    content = "class".join(sections)
    content = re.sub(r"\bnumpy\.bool\b", "numpy.bool_", content)

    # pybind11_stubgen emits `typing.SupportsFloat | typing.SupportsIndex` for
    # every `double` arg because pybind11's float coercion accepts ints. The
    # SupportsIndex branch is semantically misleading for timeout/duration
    # arguments — narrow to plain SupportsFloat.
    content = re.sub(
        r"timeout: typing\.SupportsFloat \| typing\.SupportsIndex",
        "timeout: typing.SupportsFloat",
        content,
    )

    with open("src/wujihandpy/_core/__init__.pyi", "w", encoding="utf-8") as file:
        file.write(content)


def post_process_tactile_platform_guard():
    """Mirror the runtime Linux-only gate in the stub.

    `tactile` is a Linux-only submodule (gated by `WUJIHANDPY_ENABLE_TACTILE`
    in the C++ binding and by a try/except in `wujihandpy/__init__.py`).
    pybind11_stubgen emits an unconditional `from . import tactile`, which
    makes type-checkers on Windows/macOS believe the symbol is available.
    Wrap it in a `sys.platform == 'linux'` guard and split `__all__`
    accordingly. No-op on non-Linux stub generation, where the line isn't
    emitted in the first place.
    """
    path = "src/wujihandpy/_core/__init__.pyi"
    with open(path, "r", encoding="utf-8") as file:
        content = file.read()

    if "from . import tactile" not in content:
        return  # tactile not in this build's stub — nothing to guard

    if "import sys" not in content:
        # Inject `import sys` after the `from __future__` line (always present).
        content = content.replace(
            "from __future__ import annotations\n",
            "from __future__ import annotations\nimport sys\n",
            1,
        )

    # Drop the unconditional tactile import; we'll re-emit it under the guard
    # together with __all__ in a single block.
    content = content.replace("from . import tactile\n", "", 1)

    def _split_all(match: re.Match) -> str:
        with_tactile = match.group(1)
        without_tactile = with_tactile.replace(", 'tactile'", "").replace("'tactile', ", "")
        return (
            "if sys.platform == 'linux':\n"
            "    from . import tactile\n"
            "    __all__: list[str] = " + with_tactile + "\n"
            "else:\n"
            "    __all__: list[str] = " + without_tactile + "\n"
        )

    content = re.sub(
        r"^__all__: list\[str\] = (\[[^\]]*'tactile'[^\]]*\])\n",
        _split_all,
        content,
        count=1,
        flags=re.MULTILINE,
    )

    with open(path, "w", encoding="utf-8") as file:
        file.write(content)


def post_process_tactile_callbacks():
    """Tighten `Callable` -> typed callbacks in tactile.pyi.

    pybind11_stubgen emits `collections.abc.Callable` (no parameter types) for
    pybind11 std::function bindings; for our two well-known callbacks we know
    the exact signature and want it surfaced in IDEs/type-checkers.

    No-op on builds that don't ship the tactile bindings (non-Linux, or
    Linux with `WUJIHANDPY_ENABLE_TACTILE=OFF`) — `_core/tactile.pyi` won't
    have been emitted, so we skip silently.
    """
    path = "src/wujihandpy/_core/tactile.pyi"
    try:
        with open(path, "r", encoding="utf-8") as file:
            content = file.read()
    except FileNotFoundError:
        return
    content = content.replace(
        "def set_disconnect_callback(self, callback: collections.abc.Callable) -> None:",
        "def set_disconnect_callback(self, callback: collections.abc.Callable[[], None]) -> None:",
    )
    content = content.replace(
        "def start_streaming(self, callback: collections.abc.Callable) -> None:",
        "def start_streaming(self, callback: collections.abc.Callable[[Frame], None]) -> None:",
    )
    with open(path, "w", encoding="utf-8") as file:
        file.write(content)


if __name__ == "__main__":
    main()
