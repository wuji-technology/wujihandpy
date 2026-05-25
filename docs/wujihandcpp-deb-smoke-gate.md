# wujihandcpp deb smoke gate

## Goal

The deb smoke gate verifies that a newly built `wujihandcpp` `.deb` can be installed and consumed before a pull request is merged or a release is published.

The gate is designed to catch packaging regressions such as:

- the package cannot be installed with `apt-get install ./wujihandcpp-*.deb`;
- the package omits required runtime or development dependencies;
- the package installs files owned by third-party system packages, especially `spdlog`;
- downstream CMake consumers cannot use `find_package(wujihandcpp REQUIRED)`;
- the installed shared library cannot be linked or loaded by a small consumer binary.

## CI coverage

The shared package build job lives in `.github/workflows/wujihandcpp-package-build.yml`. The PR gate in `.github/workflows/wujihandcpp-deb-package.yml` and the release workflow in `.github/workflows/release-package.yml` both call that reusable workflow so pull requests and releases build the `.deb` through the same job definition.

It runs on pull requests and pushes to `main` when package-related files change:

- `.github/scripts/smoke_wujihandcpp_deb.sh`
- `.github/workflows/wujihandcpp-deb-package.yml`
- `.github/workflows/wujihandcpp-package-build.yml`
- `.github/workflows/release-package.yml`
- `Dockerfile.package-builder`
- `wujihandcpp/**`

A release cannot publish if the deb smoke gate fails.

## Test matrix

The workflow builds the deb on both Linux runners used by the package job:

| Dimension | Values | Reason |
| --- | --- | --- |
| Runner | `ubuntu-latest`, `ubuntu-24.04-arm` | Covers amd64 and arm64 package builds. |
| Environment | host runner, `ubuntu:24.04` container | Host validates the actual GitHub runner path; the container gives a cleaner Ubuntu install surface. |
| System spdlog | absent, preinstalled `libspdlog-dev` | Catches both missing dependency behavior and file conflicts with distro-owned spdlog files. |

This produces eight smoke-test combinations per deb gate run.

## Smoke assertions

The smoke script is `.github/scripts/smoke_wujihandcpp_deb.sh`.

For each `.deb`, it performs these checks:

1. Resolve exactly one deb artifact from the build output.
2. Start from a clean `wujihandcpp` install state.
3. Optionally preinstall `libspdlog-dev`.
4. Install the local package with `apt-get install -y --no-install-recommends ./wujihandcpp-*.deb`.
5. Fail if `dpkg -L wujihandcpp` contains spdlog-owned files, including:
   - `include/spdlog`
   - `libspdlog`
   - `cmake/spdlog`
   - `pkgconfig/spdlog.pc`
6. Configure a temporary downstream CMake project with `find_package(wujihandcpp REQUIRED)`.
7. Link the consumer against `wujihandcpp::wujihandcpp`.
8. Build and run the consumer binary.

The consumer binary exercises a small public surface from the installed package:

- `wujihandcpp::data::FirmwareVersionData`
- `wujihandcpp::filter::LowPass`
- `wujihandcpp::logging`

## Reliability rationale

The host runner check is closest to what GitHub Actions actually executes during packaging and release.

The minimal Ubuntu container check is intentionally stricter. GitHub runners can have preinstalled packages that hide missing dependencies or ownership conflicts. The container keeps that failure mode visible.

The `libspdlog-dev` preinstall case specifically protects against the regression where `wujihandcpp` bundles and installs spdlog files into system locations. In that case, `apt-get install ./wujihandcpp-*.deb` fails with a file overwrite conflict.

The no-system-spdlog case protects the complementary path: a bad package might install successfully on a clean system while still shipping third-party spdlog files. The `dpkg -L` assertion catches that package ownership problem directly.

## Local reproduction

Run the script directly on an Ubuntu host:

```bash
.github/scripts/smoke_wujihandcpp_deb.sh /path/to/wujihandcpp-*.deb false
.github/scripts/smoke_wujihandcpp_deb.sh /path/to/wujihandcpp-*.deb true
```

Run the cleaner container path:

```bash
docker run --rm \
  -v "$PWD:$PWD" \
  -w "$PWD" \
  ubuntu:24.04 \
  .github/scripts/smoke_wujihandcpp_deb.sh build/wujihandcpp/wujihandcpp-*.deb false
```

Use `true` as the second argument to preinstall `libspdlog-dev` before installing the local deb.

## Non-goals

This gate does not replace full SDK tests, ROS 2 downstream integration tests, or RPM package validation. It is narrowly scoped to whether the produced Debian package is installable and consumable as a system CMake package.
