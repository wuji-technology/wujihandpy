from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / ".github" / "workflows" / "ci-ros2-downstream.yml"
SCRIPT = ROOT / "ci" / "check_wujihandros2_downstream.sh"


def test_ros2_downstream_workflow_is_quality_gate_scoped_to_prs():
    text = WORKFLOW.read_text()

    assert 'name: "Quality Gates: ROS 2 Downstream"' in text
    assert "pull_request:" in text
    assert "workflow_dispatch:" in text
    assert "push:" not in text
    assert ".github/workflows/wujihandcpp-package-build.yml" in text
    assert "ci/check_wujihandros2_downstream.sh" in text
    assert "wujihandcpp/**" in text


def test_ros2_downstream_workflow_targets_lts_matrix():
    text = WORKFLOW.read_text()

    assert "ros:humble-ros-base" in text
    assert 'ubuntu: "22.04"' in text
    assert "ros:jazzy-ros-base" in text
    assert 'ubuntu: "24.04"' in text
    assert "ros:lyrical-ros-base" not in text
    assert 'ubuntu: "26.04"' not in text
    assert "continue-on-error" not in text
    assert "kilted" not in text


def test_ros2_downstream_workflow_notes_future_2604_support():
    text = WORKFLOW.read_text()

    assert "ROS 2 Lyrical / Ubuntu 26.04" in text
    assert "once the official" in text
    assert "wujihandros2 downstream dependencies are supported" in text


def test_ros2_downstream_workflow_uses_shared_script_and_deb_artifact():
    text = WORKFLOW.read_text()

    assert "pull_request:" in text
    assert "uses: ./.github/workflows/wujihandcpp-package-build.yml" in text
    assert "actions/download-artifact" in text
    assert "build-wujihandcpp-ubuntu-latest" in text
    assert "docker/build-push-action" not in text
    assert "actions/upload-artifact" not in text
    assert "./ci/check_wujihandros2_downstream.sh" in text


def test_ros2_downstream_script_documents_downstream_build_steps():
    text = SCRIPT.read_text()

    assert "apt-get install -y" in text
    assert "dpkg -s wujihandcpp" in text
    assert "find_package(wujihandcpp CONFIG REQUIRED)" in text
    assert "git clone --depth 1" in text
    assert "--recurse-submodules" not in text
    assert "submodule update --init --remote --depth 1" in text
    assert "wujihandros2" in text
    assert "rosdep install" in text
    assert "colcon build" in text


def test_ros2_downstream_script_sources_ros_setup_without_nounset():
    text = SCRIPT.read_text()

    source = '. "/opt/ros/${ROS_DISTRO}/setup.bash"'
    pre = text.find("set +u")
    src = text.find(source, pre + 1)
    post = text.find("set -u", src + 1)
    assert pre != -1 and src != -1 and post != -1
    assert pre < src < post
