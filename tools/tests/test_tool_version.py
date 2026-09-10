#!/usr/bin/env python3
"""Tests for tools/common/tool_version.py."""

from __future__ import annotations

import re
from unittest.mock import patch

from common.tool_version import pre_commit_hook_version, probe_version

from ._helpers import completed


def test_probe_version_three_components() -> None:
    with (
        patch("common.tool_version.shutil.which", return_value="/usr/bin/clang-format"),
        patch(
            "common.tool_version.run_captured",
            return_value=completed("clang-format version 21.1.7\n"),
        ),
    ):
        assert probe_version("clang-format") == (21, 1, 7)


def test_probe_version_two_components() -> None:
    with (
        patch("common.tool_version.shutil.which", return_value="/usr/bin/foo"),
        patch("common.tool_version.run_captured", return_value=completed("foo v4.13\n")),
    ):
        assert probe_version("foo") == (4, 13)


def test_probe_version_uses_stderr_when_stdout_empty() -> None:
    with (
        patch("common.tool_version.shutil.which", return_value="/usr/bin/javac"),
        patch("common.tool_version.run_captured", return_value=completed(stderr="javac 17.0.10\n")),
    ):
        assert probe_version("javac") == (17, 0, 10)


def test_probe_version_missing_tool() -> None:
    with patch("common.tool_version.shutil.which", return_value=None):
        assert probe_version("nonexistent") is None


def test_probe_version_nonzero_exit() -> None:
    with (
        patch("common.tool_version.shutil.which", return_value="/bin/false"),
        patch("common.tool_version.run_captured", return_value=completed(returncode=1)),
    ):
        assert probe_version("false") is None


def test_probe_version_unparseable() -> None:
    with (
        patch("common.tool_version.shutil.which", return_value="/bin/x"),
        patch("common.tool_version.run_captured", return_value=completed("not a version")),
    ):
        assert probe_version("x") is None


def test_probe_version_custom_pattern() -> None:
    pattern = re.compile(r"v(\d+)-(\d+)")
    with (
        patch("common.tool_version.shutil.which", return_value="/bin/x"),
        patch("common.tool_version.run_captured", return_value=completed("release v2-7\n")),
    ):
        assert probe_version("x", pattern=pattern) == (2, 7)


def test_probe_version_custom_args() -> None:
    with (
        patch("common.tool_version.shutil.which", return_value="/bin/git"),
        patch(
            "common.tool_version.run_captured", return_value=completed("git version 2.42.0\n")
        ) as run,
    ):
        probe_version("git", args=("version",))
        run.assert_called_once()
        assert run.call_args[0][0] == ["git", "version"]


def test_pre_commit_hook_version_ruff() -> None:
    assert pre_commit_hook_version("ruff") == (0, 15, 20)


def test_pre_commit_hook_version_shares_rev_across_hooks_in_one_repo() -> None:
    assert pre_commit_hook_version("ruff-format") == pre_commit_hook_version("ruff")


def test_pre_commit_hook_version_keeps_packaging_revision() -> None:
    # shellcheck-py's rev carries a fourth component that no shellcheck release has;
    # callers compare on the shorter prefix rather than losing it here.
    assert pre_commit_hook_version("shellcheck") == (0, 11, 0, 1)


def test_pre_commit_hook_version_clang_format() -> None:
    assert pre_commit_hook_version("clang-format") == (22, 1, 5)


def test_pre_commit_hook_version_local_repo_has_no_rev() -> None:
    assert pre_commit_hook_version("qmllint") is None


def test_pre_commit_hook_version_unknown_hook() -> None:
    assert pre_commit_hook_version("no-such-hook") is None


def test_pre_commit_hook_version_missing_config(tmp_path) -> None:
    assert pre_commit_hook_version("ruff", config_path=tmp_path / "absent.yaml") is None


def test_pre_commit_hook_version_ignores_hook_name_inside_entry(tmp_path) -> None:
    config = tmp_path / ".pre-commit-config.yaml"
    config.write_text(
        "repos:\n"
        "  - repo: local\n"
        "    hooks:\n"
        "      - id: decoy\n"
        "        entry: bash -c 'echo - id: ruff'\n"
        "  - repo: https://github.com/astral-sh/ruff-pre-commit\n"
        "    rev: v9.9.9\n"
        "    hooks:\n"
        "      - id: ruff\n",
        encoding="utf-8",
    )
    assert pre_commit_hook_version("ruff", config_path=config) == (9, 9, 9)
