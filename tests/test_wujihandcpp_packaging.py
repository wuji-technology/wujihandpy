from pathlib import Path
import shutil
import subprocess


def run(command):
    subprocess.run(command, check=True, text=True)


def test_shared_standalone_install_does_not_vendor_spdlog(tmp_path):
    source_dir = Path(__file__).resolve().parents[1] / "wujihandcpp"
    build_dir = tmp_path / "build"
    install_prefix = tmp_path / "install"

    configure = [
        "cmake",
        "-S",
        str(source_dir),
        "-B",
        str(build_dir),
        f"-DCMAKE_INSTALL_PREFIX={install_prefix}",
        "-DBUILD_STATIC_WUJIHANDCPP=OFF",
        "-DBUILD_TESTING=OFF",
        "-DWUJIHANDCPP_INSTALL=ON",
    ]
    if shutil.which("gcc-13") and shutil.which("g++-13"):
        configure.extend(["-DCMAKE_C_COMPILER=gcc-13", "-DCMAKE_CXX_COMPILER=g++-13"])
    if shutil.which("ninja"):
        configure.extend(["-G", "Ninja"])

    run(configure)
    run(["cmake", "--build", str(build_dir), "--target", "install", "--parallel", "2"])

    installed_sdk_libs = list(install_prefix.glob("lib*/libwujihandcpp.*"))
    assert installed_sdk_libs

    vendored_spdlog = (
        list(install_prefix.glob("include/spdlog/**"))
        + list(install_prefix.glob("lib*/libspdlog*"))
        + list(install_prefix.glob("lib*/cmake/spdlog/**"))
        + list(install_prefix.glob("lib*/pkgconfig/spdlog.pc"))
    )
    assert vendored_spdlog == []

    targets_file = next(install_prefix.glob("lib*/cmake/wujihandcpp/wujihandcppTargets.cmake"))
    assert "spdlog::spdlog" not in targets_file.read_text()
