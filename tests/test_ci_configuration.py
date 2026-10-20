"""Keep the Windows SDK bootstrap compatible with changing runner images."""

from pathlib import Path

import yaml


def test_windows_ci_does_not_pin_visual_studio_version():
    root = Path(__file__).resolve().parents[1]
    workflow = yaml.safe_load((root / ".github/workflows/ci.yml").read_text(encoding="utf-8"))
    steps = workflow["jobs"]["test"]["steps"]
    bootstrap = next(step for step in steps if "msvc-dev-cmd" in step.get("uses", ""))
    assert not bootstrap.get("with", {}).get("vsversion"), (
        "Do not pin Visual Studio: windows-latest can change the installed version"
    )
