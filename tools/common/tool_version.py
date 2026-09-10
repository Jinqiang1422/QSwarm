"""Probe an external tool's version string into a tuple of ints.

Standardizes the ``tool --version`` + regex parse that ``ccache_helper``,
``release.py``, ``analyze.py``, and ``check_deps.py`` each implement
differently.
"""

from __future__ import annotations

import re
import shutil
from pathlib import Path
from typing import TYPE_CHECKING

from .proc import run_captured

if TYPE_CHECKING:
    from collections.abc import Sequence

__all__ = [
    "DEFAULT_VERSION_RE",
    "pre_commit_hook_version",
    "probe_version",
    "uv_lock_version",
]

DEFAULT_VERSION_RE: re.Pattern[str] = re.compile(r"(\d+)\.(\d+)(?:\.(\d+))?")

_UV_LOCK = Path(__file__).resolve().parents[1] / "uv.lock"
_PRE_COMMIT_CONFIG = Path(__file__).resolve().parents[2] / ".pre-commit-config.yaml"

# Split the config into `- repo:` blocks. Anchored to line start so `repo:` text
# inside an `entry:` or a comment cannot open a spurious block.
_REPO_BLOCK_RE = re.compile(r"^\s*-\s+repo:", re.M)
_REV_RE = re.compile(r"""^\s*rev:\s*['"]?v?([0-9][0-9.]*)['"]?\s*$""", re.M)


def uv_lock_version(package: str, *, lock_path: Path | None = None) -> str | None:
    """Return the version pinned for *package* in tools/uv.lock, or None if absent/unreadable.

    Lets a setup script pin its fallback binary to the same version the dev/CI venv
    resolves (single source of truth), so the two install paths can't drift. Returns
    None in contexts without the lockfile (e.g. the Docker builders that COPY setup
    scripts out of the repo), leaving the caller to apply its own fallback.
    """
    path = lock_path or _UV_LOCK
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return None
    match = re.search(
        r'\[\[package\]\]\s*\nname\s*=\s*"' + re.escape(package) + r'"\s*\nversion\s*=\s*"([\d.]+)"',
        text,
    )
    return match.group(1) if match else None


def pre_commit_hook_version(
    hook_id: str, *, config_path: Path | None = None
) -> tuple[int, ...] | None:
    """Return the version pinned for *hook_id*'s repo in .pre-commit-config.yaml.

    The companion of ``uv_lock_version`` for tools that come from pre-commit
    mirrors rather than the uv lockfile: it lets a checker assert that a host
    binary matches the version CI actually enforces, instead of accepting
    whatever happens to be on PATH. Returns None when the config is unreadable,
    the hook is not declared, or its repo carries no ``rev`` (``repo: local``).

    Parsed with a regex rather than PyYAML on purpose: tool_version is listed in
    tools/tests/test_common_import_isolation.py's CI-critical set and must stay
    dependency-free for system-Python callers.

    All numeric components are returned, so shellcheck-py's four-part rev
    ``v0.11.0.1`` yields ``(0, 11, 0, 1)`` -- the trailing packaging revision is
    kept for reporting and dropped by prefix comparison at the call site.
    """
    path = config_path or _PRE_COMMIT_CONFIG
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return None

    # Anchor the id match to its own list-item line: `entry:` values routinely
    # embed hook names as free text and would otherwise match.
    id_re = re.compile(r"""^\s*-\s+id:\s*['"]?""" + re.escape(hook_id) + r"""['"]?\s*$""", re.M)

    boundaries = [match.start() for match in _REPO_BLOCK_RE.finditer(text)]
    for index, start in enumerate(boundaries):
        end = boundaries[index + 1] if index + 1 < len(boundaries) else len(text)
        block = text[start:end]
        if not id_re.search(block):
            continue
        rev = _REV_RE.search(block)
        return tuple(int(part) for part in rev.group(1).split(".") if part) if rev else None
    return None


def probe_version(
    tool: str,
    *,
    args: Sequence[str] = ("--version",),
    pattern: re.Pattern[str] = DEFAULT_VERSION_RE,
    timeout: float = 5.0,
) -> tuple[int, ...] | None:
    """Run ``<tool> <args>`` and parse the first version-shaped token.

    Returns a tuple like ``(4, 13, 6)`` or ``None`` if the tool is missing,
    times out, exits non-zero, or produces no parseable version.
    Patch component is optional; missing trailing components are dropped.
    """
    if not shutil.which(tool):
        return None

    try:
        result = run_captured([tool, *args], timeout=timeout)
    except (TimeoutError, OSError):
        return None

    if result.returncode != 0:
        return None

    # version may go to stdout or stderr depending on the tool
    text = result.stdout or result.stderr
    match = pattern.search(text)
    if not match:
        return None

    return tuple(int(g) for g in match.groups() if g is not None)
