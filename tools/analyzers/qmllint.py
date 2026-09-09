"""qmllint QML linter."""

from __future__ import annotations

import re
from typing import TYPE_CHECKING, ClassVar

from common.analyzer import AnalysisResult, AnalyzerBase
from common.logging import log_error, log_info, log_warn
from common.proc import run_captured

if TYPE_CHECKING:
    from pathlib import Path


class QmlLintAnalyzer(AnalyzerBase):
    """QML file linter."""

    name: ClassVar[str] = "qmllint"
    install_hint: ClassVar[str] = (
        "Install with: Qt SDK or 'sudo apt install qt6-declarative-dev-tools'"
    )

    def run(self, files: list[Path], fix: bool = False) -> AnalysisResult:
        if not self.require_tool("qmllint"):
            return AnalysisResult(tool=self.name, passed=False, output="Tool not found")

        if not files:
            log_info("No QML files to lint")
            return AnalysisResult(tool=self.name, passed=True)

        version = run_captured(["qmllint", "--version"])
        version_output = version.stdout + version.stderr
        if version.returncode != 0 or not re.search(r"\bqmllint\s+6\.", version_output):
            message = "Qt 6 qmllint is required; put the Qt SDK bin directory first in PATH"
            log_error(message)
            return AnalysisResult(tool=self.name, passed=False, output=message)

        log_info(f"Running qmllint on {len(files)} files...")

        issues_found = False
        files_with_issues: list[str] = []
        all_output: list[str] = []

        command = ["qmllint"]
        qml_imports = self.build_dir / "qml"
        if qml_imports.is_dir():
            command.extend(["-I", str(qml_imports)])

        for file in files:
            result = run_captured([*command, str(file)])

            output = result.stdout + result.stderr
            if output.strip():
                print(output)
                all_output.append(output)

            if result.returncode != 0:
                issues_found = True
                files_with_issues.append(self.relative_path(file))

        if issues_found:
            log_warn("QML lint issues found")

        return AnalysisResult(
            tool=self.name,
            passed=not issues_found,
            issues=len(files_with_issues),
            output="\n".join(all_output),
            files_checked=len(files),
            files_with_issues=files_with_issues,
        )
