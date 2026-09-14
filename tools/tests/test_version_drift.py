#!/usr/bin/env python3
"""Drift guards for the tools venv.

uv.lock vs pyproject.toml: a dep added without regenerating the lockfile makes
CI's `uv sync --frozen` fail with a confusing error — fail locally instead via
`uv lock --locked`, which exits non-zero when the lockfile would change.

JUST_VERSION vs rust-just: two independent install paths deliver `just` — the
upstream-binary fallback in install_dependencies/_debian.py (Debian/Ubuntu
< 24.04) and rust-just from PyPI (dev/CI venv via uv). _debian derives its
JUST_VERSION from uv.lock's rust-just at runtime so the two can't drift; this
guards that the derivation still resolves against the real lockfile.
"""

from __future__ import annotations

import re
import shutil
import subprocess

import pytest

from ._helpers import TOOLS_DIR, load_script_module

UV_LOCK = TOOLS_DIR / "uv.lock"


def test_uv_lock_in_sync_with_pyproject() -> None:
    if not UV_LOCK.exists():
        pytest.skip("tools/uv.lock not present")
    if not shutil.which("uv"):
        pytest.skip("uv not installed; CI must validate lockfile drift via its own uv install")

    result = subprocess.run(
        ["uv", "lock", "--locked", "--project", str(TOOLS_DIR)],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        pytest.fail(
            "tools/uv.lock is out of sync with tools/pyproject.toml.\n"
            "Run: (cd tools && uv lock) and commit the result.\n\n"
            f"uv stderr:\n{result.stderr}"
        )


def _lock_version(package: str) -> str:
    """Independently parse *package*'s pin from uv.lock (not via the helper under test)."""
    text = UV_LOCK.read_text(encoding="utf-8")
    match = re.search(
        r'\[\[package\]\]\s*\nname\s*=\s*"'
        + re.escape(package)
        + r'"\s*\nversion\s*=\s*"([\d.]+)"',
        text,
    )
    assert match, f"{package} package not found in tools/uv.lock"
    return match.group(1)


def _derived_versions() -> dict[str, str]:
    """The version each setup script resolves for its pip/binary fallback tool."""
    from setup.install_dependencies._debian import JUST_VERSION

    bg = load_script_module("setup/build-gstreamer.py", "build_gstreamer")
    return {"rust-just": JUST_VERSION, "meson": bg.MESON_VERSION, "ninja": bg.NINJA_VERSION}


@pytest.mark.parametrize("package", ["rust-just", "meson", "ninja"])
def test_fallback_version_derives_from_uv_lock(package: str) -> None:
    if not UV_LOCK.exists():
        pytest.skip("tools/uv.lock not in checkout")

    derived = _derived_versions()[package]
    pinned = _lock_version(package)

    assert derived == pinned, (
        f"{package}: setup script resolved {derived!r} but tools/uv.lock pins {pinned!r}. "
        f"common.tool_version.uv_lock_version is broken or the constant's fallback is "
        f"shadowing the lock, so the fallback-install path would ship a stale binary."
    )


def _config_rev(hook_id: str) -> str:
    """Independently parse *hook_id*'s repo rev from .pre-commit-config.yaml."""
    text = (TOOLS_DIR.parent / ".pre-commit-config.yaml").read_text(encoding="utf-8")
    blocks = re.split(r"^\s*-\s+repo:", text, flags=re.M)
    for block in blocks:
        if not re.search(rf"^\s*-\s+id:\s*{re.escape(hook_id)}\s*$", block, re.M):
            continue
        rev = re.search(r"""^\s*rev:\s*['"]?v?([0-9][0-9.]*)['"]?\s*$""", block, re.M)
        assert rev, f"hook {hook_id} found but its repo carries no rev"
        return rev.group(1)
    pytest.fail(f"hook {hook_id} not declared in .pre-commit-config.yaml")


@pytest.mark.parametrize("hook_id", ["ruff", "ruff-format", "shellcheck", "clang-format"])
def test_pre_commit_pins_resolve_for_gated_hooks(hook_id: str) -> None:
    from common.tool_version import pre_commit_hook_version

    derived = pre_commit_hook_version(hook_id)
    pinned = tuple(int(part) for part in _config_rev(hook_id).split("."))

    assert derived == pinned, (
        f"{hook_id}: pre_commit_hook_version resolved {derived!r} but "
        f".pre-commit-config.yaml pins {pinned!r}. swarm_ext/tests/run_all.py's "
        f"_pinned_tool gate depends on these pins; when one stops resolving, the L0 "
        f"lint gates all turn BLOCKED and the harness reports exit code 2."
    )
