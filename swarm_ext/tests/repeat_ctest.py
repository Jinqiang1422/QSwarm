#!/usr/bin/env python3
"""Repeat a CI-equivalent CTest selection with one JUnit and log per attempt."""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


def _ctest_command(args: argparse.Namespace, junit_path: Path) -> list[str]:
    command = [
        "ctest",
        "--no-tests=error",
        "--output-on-failure",
        "--output-junit",
        str(junit_path),
        "--parallel",
        str(args.jobs),
    ]
    if args.include_labels:
        command.extend(["-L", args.include_labels])
    if args.exclude_labels:
        command.extend(["-LE", args.exclude_labels])
    if args.test_regex:
        command.extend(["-R", args.test_regex])
    if sys.platform.startswith("linux") and not os.environ.get("DISPLAY"):
        xvfb_run = shutil.which("xvfb-run")
        if xvfb_run:
            command = [xvfb_run, "-a", *command]
    return command


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--repeat", type=int, required=True)
    parser.add_argument("--jobs", type=int, required=True)
    parser.add_argument("--include-labels", default="")
    parser.add_argument("--exclude-labels", default="Flaky|Network")
    parser.add_argument("--test-regex", default="")
    parser.add_argument("--timeout", type=float, default=7200.0)
    args = parser.parse_args()
    if args.repeat < 1 or args.jobs < 1 or args.timeout <= 0:
        parser.error("repeat, jobs, and timeout must be positive")
    if not shutil.which("ctest"):
        parser.error("ctest is required")
    build_dir = Path(args.build_dir).resolve()
    if not (build_dir / "CTestTestfile.cmake").is_file():
        parser.error(f"not a configured CTest build: {build_dir}")
    output_dir = Path(args.out).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    rows = []
    overall_started = time.monotonic()
    started_at = datetime.now(timezone.utc).isoformat()
    for attempt in range(1, args.repeat + 1):
        junit_path = output_dir / f"attempt-{attempt:03d}.xml"
        log_path = output_dir / f"attempt-{attempt:03d}.log"
        command = _ctest_command(args, junit_path)
        started = time.monotonic()
        timed_out = False
        with log_path.open("w", encoding="utf-8") as log_file:
            process = subprocess.Popen(
                command,
                cwd=build_dir,
                stdout=log_file,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            try:
                return_code = process.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(process.pid, signal.SIGTERM)
                try:
                    return_code = process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=5)
                return_code = 124
        if timed_out:
            with log_path.open("a", encoding="utf-8") as log_file:
                log_file.write(f"\nrepeat_ctest timeout after {args.timeout:.0f}s\n")
        duration = round(time.monotonic() - started, 3)
        row = {
            "attempt": attempt,
            "return_code": return_code,
            "timed_out": timed_out,
            "duration_sec": duration,
            "junit": str(junit_path) if junit_path.is_file() else "",
            "log": str(log_path),
            "pass": return_code == 0,
        }
        rows.append(row)
        print(
            f"[{attempt}/{args.repeat}] {'PASS' if row['pass'] else 'FAIL'} "
            f"{duration:.3f}s log={log_path.name}",
            flush=True,
        )
        if not row["pass"]:
            break

    payload = {
        "version": 1,
        "started_at": started_at,
        "finished_at": datetime.now(timezone.utc).isoformat(),
        "repeat_requested": args.repeat,
        "repeat_completed": len(rows),
        "duration_sec": round(time.monotonic() - overall_started, 3),
        "selection": {
            "include_labels": args.include_labels,
            "exclude_labels": args.exclude_labels,
            "test_regex": args.test_regex,
            "jobs": args.jobs,
        },
        "results": rows,
        "pass": len(rows) == args.repeat and all(row["pass"] for row in rows),
    }
    report_path = output_dir / "report.json"
    report_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"report={report_path}")
    return 0 if payload["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
