"""Regression coverage for Windows runner images without Visual Studio 2022."""

from pathlib import Path

import yaml


def test_windows_ci_accepts_an_image_with_only_visual_studio_2026():
    root = Path(__file__).resolve().parents[1]
    workflow = yaml.safe_load((root / ".github/workflows/ci.yml").read_text(encoding="utf-8"))
    steps = workflow["jobs"]["test"]["steps"]
    bootstrap = next(step for step in steps if "msvc-dev-cmd" in step.get("uses", ""))
    requested_version = str(bootstrap.get("with", {}).get("vsversion", ""))
    assert requested_version in {"", "2026", "18", "18.0"}, (
        "windows-latest may contain only VS 2026; do not require VS 2022 to load the SDK"
    )
    configure = next(
        step
        for step in steps
        if "cmake --preset" in step.get("run", "") and step.get("if") == "runner.os == 'Windows'"
    )
    assert configure.get("env", {}).get("CC") == "clang-cl"
    assert configure.get("env", {}).get("CXX") == "clang-cl", (
        "Use the supported clang-cl compiler when loading a newer Visual Studio SDK environment"
    )
