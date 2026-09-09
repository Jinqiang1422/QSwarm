"""Regression tests for analyzer failures and diagnostic reporting."""

from pathlib import Path
from subprocess import CompletedProcess
from unittest.mock import patch

import pytest
from analyzers.clang_tidy import ClangTidyAnalyzer
from analyzers.clazy import ClazyAnalyzer
from analyzers.qmllint import QmlLintAnalyzer


@pytest.mark.parametrize("analyzer_type", [ClangTidyAnalyzer, ClazyAnalyzer, QmlLintAnalyzer])
def test_missing_tool_fails(analyzer_type, tmp_path: Path) -> None:
    (tmp_path / "compile_commands.json").write_text("[]")
    analyzer = analyzer_type(tmp_path, tmp_path)
    with patch("common.analyzer.shutil.which", return_value=None):
        result = analyzer.run([tmp_path / "source.cc"])
    assert not result.passed
    assert result.files_checked == 0
    assert result.output == "Tool not found"


@pytest.mark.parametrize("analyzer_type", [ClangTidyAnalyzer, ClazyAnalyzer])
def test_missing_compilation_database_fails(analyzer_type, tmp_path: Path) -> None:
    result = analyzer_type(tmp_path, tmp_path).run([tmp_path / "source.cc"])
    assert not result.passed
    assert result.files_checked == 0
    assert "compile_commands.json" in result.output


@pytest.mark.parametrize(
    "code,stdout,stderr,passed",
    [
        (0, "", "", True),
        (1, "", "", False),
        (0, "source.cc:1: warning: Qt misuse [-Wclazy-connect-non-signal]", "", False),
        (0, "", "source.cc:1: warning: Qt misuse [-Wclazy-connect-non-signal]", False),
        (1, "", "source.cc:1: error: invalid C++", False),
    ],
)
def test_clazy_diagnostics(
    tmp_path: Path, code: int, stdout: str, stderr: str, passed: bool
) -> None:
    (tmp_path / "compile_commands.json").write_text("[]")
    with (
        patch("common.analyzer.shutil.which", return_value="clazy-standalone"),
        patch(
            "analyzers.clazy.run_captured", return_value=CompletedProcess([], code, stdout, stderr)
        ),
    ):
        result = ClazyAnalyzer(tmp_path, tmp_path).run([tmp_path / "source.cc"])
    assert result.passed is passed
    assert result.files_checked == 1
    assert result.files_with_issues == ([] if passed else ["source.cc"])
    assert result.output == stdout + stderr


def test_clang_tidy_preserves_diagnostics(tmp_path: Path, capsys) -> None:
    (tmp_path / "compile_commands.json").write_text("[]")
    diagnostic = "source.cc:1: error: unsafe access"
    with (
        patch("common.analyzer.shutil.which", return_value="clang-tidy"),
        patch(
            "analyzers.clang_tidy.run_captured",
            return_value=CompletedProcess([], 1, diagnostic, ""),
        ),
    ):
        result = ClangTidyAnalyzer(tmp_path, tmp_path).run([tmp_path / "source.cc"])
    assert not result.passed
    assert result.output == diagnostic
    assert diagnostic in capsys.readouterr().out


@pytest.mark.parametrize("version", ["qmllint 1.0", "qmllint 5.15.2", "unrecognized version"])
def test_qmllint_rejects_incompatible_tool(tmp_path: Path, version: str) -> None:
    with (
        patch("common.analyzer.shutil.which", return_value="qmllint"),
        patch(
            "analyzers.qmllint.run_captured", return_value=CompletedProcess([], 0, version, "")
        ) as run,
    ):
        result = QmlLintAnalyzer(tmp_path, tmp_path).run([tmp_path / "main.qml"])
    assert not result.passed
    assert result.files_checked == 0
    assert "Qt 6" in result.output
    run.assert_called_once_with(["qmllint", "--version"])


def test_qmllint_reports_failure_in_second_file(tmp_path: Path) -> None:
    files = [tmp_path / "first.qml", tmp_path / "second.qml"]
    (tmp_path / "qml").mkdir()
    with (
        patch("common.analyzer.shutil.which", return_value="qmllint"),
        patch(
            "analyzers.qmllint.run_captured",
            side_effect=[
                CompletedProcess([], 0, "qmllint 6.11.1\n", ""),
                CompletedProcess([], 0, "", ""),
                CompletedProcess([], 1, "", "second.qml: syntax error"),
            ],
        ) as run,
    ):
        result = QmlLintAnalyzer(tmp_path, tmp_path).run(files)
    assert not result.passed
    assert result.files_checked == 2
    assert result.files_with_issues == ["second.qml"]
    assert run.call_args_list[-1].args[0] == ["qmllint", "-I", str(tmp_path / "qml"), str(files[1])]
