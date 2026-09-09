#!/usr/bin/env python3
"""Tests for tools/analyze.py."""

from __future__ import annotations

import subprocess
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest
from analyze import FileCollector, get_analyzer, main, validate_path
from common.analyzer import AnalysisResult


class TestFileCollector:
    def test_get_compare_ref_symbolic(self, tmp_path: Path) -> None:
        collector = FileCollector(tmp_path)
        mock_result = MagicMock(returncode=0, stdout="origin/main\n")
        with patch("common.proc.subprocess.run", return_value=mock_result):
            ref = collector.get_compare_ref()
        assert ref == "main"

    def test_get_compare_ref_fallback_master(self, tmp_path: Path) -> None:
        collector = FileCollector(tmp_path)
        symbolic_fail = MagicMock(returncode=1, stdout="")
        master_ok = MagicMock(returncode=0)

        def side_effect(cmd, **kw):
            if "symbolic-ref" in cmd:
                return symbolic_fail
            if cmd[-1] == "master":
                return master_ok
            return MagicMock(returncode=1)

        with patch("common.proc.subprocess.run", side_effect=side_effect):
            ref = collector.get_compare_ref()
        assert ref == "master"

    def test_get_compare_ref_none(self, tmp_path: Path) -> None:
        collector = FileCollector(tmp_path)
        fail = MagicMock(returncode=1, stdout="")
        with patch("common.proc.subprocess.run", return_value=fail):
            ref = collector.get_compare_ref()
        assert ref is None

    def test_find_files(self, tmp_path: Path) -> None:
        src = tmp_path / "src"
        src.mkdir()
        (src / "foo.cpp").touch()
        (src / "bar.h").touch()
        (src / "readme.txt").touch()

        collector = FileCollector(tmp_path)
        files = collector._find_files(src, (".cpp", ".h"))
        names = [f.name for f in files]
        assert "foo.cpp" in names
        assert "bar.h" in names
        assert "readme.txt" not in names

    def test_explicit_file(self, tmp_path: Path) -> None:
        source = tmp_path / "vehicle.cc"
        source.touch()
        collector = FileCollector(tmp_path)
        assert collector.get_cpp_files(Path(source.name)) == [source]
        assert collector.get_qml_files(Path(source.name)) == []

    def test_changed_files_include_worktree_and_untracked(self, tmp_path: Path) -> None:
        def git(*args: str) -> None:
            subprocess.run(["git", *args], cwd=tmp_path, check=True, capture_output=True)

        git("init")
        for name in ["committed.cc", "staged.cc", "working.cc", "deleted.cc"]:
            (tmp_path / name).write_text("before\n")
        git("add", ".")
        git("-c", "user.name=Test", "-c", "user.email=test@example.com", "commit", "-m", "baseline")
        git("branch", "baseline")
        (tmp_path / "committed.cc").write_text("after\n")
        git("add", "committed.cc")
        git("-c", "user.name=Test", "-c", "user.email=test@example.com", "commit", "-m", "change")
        (tmp_path / "staged.cc").write_text("after\n")
        git("add", "staged.cc")
        (tmp_path / "working.cc").write_text("after\n")
        (tmp_path / "deleted.cc").unlink()
        (tmp_path / "new file.cc").touch()
        (tmp_path / "unrelated.qml").touch()
        files = FileCollector(tmp_path)._get_changed_files((".cc",), "baseline")
        assert {f.name for f in files} == {"committed.cc", "staged.cc", "working.cc", "new file.cc"}

    def test_failed_git_scan_is_not_an_empty_success(self, tmp_path: Path) -> None:
        with (
            patch("analyze.run_git", return_value=MagicMock(returncode=1, stderr="bad ref")),
            pytest.raises(RuntimeError, match="bad ref"),
        ):
            FileCollector(tmp_path)._get_changed_files((".cc",), "missing")


class TestValidatePath:
    def test_valid_relative_path(self, tmp_path: Path) -> None:
        (tmp_path / "src").mkdir()
        result = validate_path("src", tmp_path)
        assert result == Path("src")

    def test_rejects_parent_traversal(self, tmp_path: Path) -> None:
        import pytest

        with pytest.raises(ValueError, match="must not contain"):
            validate_path("../etc/passwd", tmp_path)

    def test_rejects_absolute_path(self, tmp_path: Path) -> None:
        import pytest

        with pytest.raises(ValueError, match="must be relative"):
            validate_path("/etc/passwd", tmp_path)

    def test_rejects_missing_path(self, tmp_path: Path) -> None:
        with pytest.raises(ValueError, match="does not exist"):
            validate_path("missing.qml", tmp_path)

    def test_rejects_symlink_outside_repository(self, tmp_path: Path) -> None:
        repo = tmp_path / "repo"
        repo.mkdir()
        (repo / "outside").symlink_to(tmp_path, target_is_directory=True)
        with pytest.raises(ValueError, match="outside repository"):
            validate_path("outside", repo)

    def test_allows_dots_in_filename(self, tmp_path: Path) -> None:
        (tmp_path / "test..qml").touch()
        assert validate_path("test..qml", tmp_path) == Path("test..qml")


@pytest.mark.parametrize("tool,suffix", [("qmllint", ".qml"), ("clazy", ".cc")])
def test_main_analyzes_all_explicit_paths(tmp_path: Path, tool: str, suffix: str) -> None:
    folder = tmp_path / "src"
    folder.mkdir()
    first = folder / f"first{suffix}"
    second = folder / f"second{suffix}"
    first.touch()
    second.touch()
    (folder / "unrelated.txt").touch()
    analyzer = MagicMock()
    analyzer.run.return_value = AnalysisResult(tool=tool, passed=True)
    with (
        patch("sys.argv", ["analyze.py", "--tool", tool, f"src/first{suffix}", "src"]),
        patch("analyze.find_repo_root", return_value=tmp_path),
        patch("analyze.get_analyzer", return_value=analyzer),
        patch.object(
            FileCollector, "get_compare_ref", side_effect=AssertionError("unexpected git scan")
        ),
    ):
        assert main() == 0
    analyzer.run.assert_called_once_with([first, second], fix=False)


def test_main_rejects_conflicting_scope(tmp_path: Path) -> None:
    with (
        patch("sys.argv", ["analyze.py", "--all", "src"]),
        patch("analyze.find_repo_root", return_value=tmp_path),
        patch("analyze.get_analyzer") as factory,
    ):
        assert main() == 1
    factory.assert_not_called()


class TestGetAnalyzer:
    def test_returns_clang_tidy(self, tmp_path: Path) -> None:
        analyzer = get_analyzer("clang-tidy", tmp_path, tmp_path / "build", jobs=4)
        assert analyzer.name == "clang-tidy"
        assert analyzer.jobs == 4  # type: ignore[attr-defined]

    def test_returns_clazy_with_jobs(self, tmp_path: Path) -> None:
        analyzer = get_analyzer("clazy", tmp_path, tmp_path / "build", jobs=2)
        assert analyzer.name == "clazy"
        assert analyzer.jobs == 2  # type: ignore[attr-defined]

    def test_returns_cppcheck(self, tmp_path: Path) -> None:
        analyzer = get_analyzer("cppcheck", tmp_path, tmp_path / "build")
        assert analyzer.name == "cppcheck"

    def test_unknown_tool_raises(self, tmp_path: Path) -> None:
        import pytest

        with pytest.raises(ValueError, match="Unknown tool"):
            get_analyzer("nonexistent", tmp_path, tmp_path / "build")
