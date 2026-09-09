"""clazy Qt-specific analyzer."""

from __future__ import annotations

import re
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import TYPE_CHECKING, ClassVar

from common.analyzer import AnalysisResult, AnalyzerBase
from common.logging import log_info, log_warn
from common.proc import run_captured

if TYPE_CHECKING:
    from pathlib import Path


class ClazyAnalyzer(AnalyzerBase):
    """Clazy Qt-specific static analyzer."""

    name: ClassVar[str] = "clazy"
    install_hint: ClassVar[str] = "Install with: sudo apt install clazy"
    CHECKS: ClassVar[str] = "level1,connect-non-signal,lambda-in-connect,overridden-signal"

    def __init__(self, repo_root: Path, build_dir: Path, jobs: int = 1) -> None:
        super().__init__(repo_root, build_dir)
        self.jobs = jobs

    def _analyze_file(self, file: Path) -> tuple[str, bool, str]:
        rel_path = self.relative_path(file)
        result = run_captured(
            [
                "clazy-standalone",
                "-p",
                str(self.build_dir),
                f"--checks={self.CHECKS}",
                str(file),
            ],
        )
        output = result.stdout + result.stderr
        # Clazy emits diagnostics with exit status zero unless compilation fails.
        has_issues = result.returncode != 0 or bool(re.search(r"\b(?:warning|error):", output))
        return rel_path, has_issues, output

    def run(self, files: list[Path], fix: bool = False) -> AnalysisResult:
        if not self.require_compile_commands():
            return AnalysisResult(
                tool=self.name, passed=False, output="compile_commands.json not found"
            )

        if not self.require_tool("clazy-standalone"):
            return AnalysisResult(tool=self.name, passed=False, output="Tool not found")

        if not files:
            log_info("No files to analyze")
            return AnalysisResult(tool=self.name, passed=True)

        log_info(f"Running clazy on {len(files)} files ({self.jobs} jobs)...")

        files_with_issues: list[str] = []
        all_output: list[str] = []

        with ThreadPoolExecutor(max_workers=self.jobs) as pool:
            futures = {pool.submit(self._analyze_file, f): f for f in files}
            for future in as_completed(futures):
                rel_path, has_issues, stderr = future.result()
                if has_issues:
                    log_warn(f"Issues in {rel_path}:")
                    print(stderr)
                    all_output.append(stderr)
                    files_with_issues.append(rel_path)

        if files_with_issues:
            log_warn("Clazy found Qt-specific issues")

        return AnalysisResult(
            tool=self.name,
            passed=not files_with_issues,
            issues=len(files_with_issues),
            output="\n".join(all_output),
            files_checked=len(files),
            files_with_issues=files_with_issues,
        )
