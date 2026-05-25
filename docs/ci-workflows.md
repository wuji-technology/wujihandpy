# CI workflows

This repository keeps GitHub Actions workflows grouped by the product stage they protect.

| Workflow name | File | Trigger | Purpose |
| --- | --- | --- | --- |
| `Quality Gates: Python Tests` | `.github/workflows/ci-python-tests.yml` | Pull requests and pushes to `main` | Builds the editable Python package and runs host-side pytest. |
| `Quality Gates: C++11 Header Compatibility` | `.github/workflows/ci-cpp11-header-compatibility.yml` | Pull requests and pushes to `main` that touch `wujihandcpp/include/**` | Verifies public headers remain consumable by the C++11 compatibility probe. |
| `Quality Gates: wujihandcpp Deb Package` | `.github/workflows/pr-wujihandcpp-deb-gate.yml` | Pull requests that touch package-related files, plus manual dispatch | Builds the deb and smoke-tests installation and downstream CMake consumption before merge. |
| `Quality Gates: ROS 2 Downstream` | `.github/workflows/ci-ros2-downstream.yml` | Pull requests that touch ROS 2 downstream or package-build inputs, plus manual dispatch | Builds the deb and validates wujihandros2 on supported ROS 2 LTS images. ROS 2 Lyrical / Ubuntu 26.04 is noted in the workflow and should be added to the matrix after official image and downstream dependency support lands. |
| `Reusable: wujihandcpp Package Build` | `.github/workflows/wujihandcpp-package-build.yml` | `workflow_call` only | Shared package build implementation used by PR and release workflows. |
| `Docs: Notify Docs Center` | `.github/workflows/docs-notify.yml` | Pull requests or pushes that touch `docs/external/**` | Sends docs preview and publish notifications to the docs center repository. |
| `Release` | `.github/workflows/release.yml` | Merged release PRs and `v*` tags | Creates release tags and publishes release artifacts. |

## Naming conventions

- `Quality Gates:` workflows are validation jobs that protect pull requests and the protected `main` branch.
- `Reusable:` workflows are not standalone checks; they are called by other workflows.
- `Docs:` workflows only coordinate external documentation publishing.
- `Release` owns the release lifecycle from tag creation to artifact publishing.

GitHub's Actions page lists every workflow file in the repository, even if a workflow did not run for the current pull request. For pull request review, check the PR status checks first; they show only the workflows triggered by that PR.
