#!/usr/bin/env python3
"""Verify that a non-test QGC artifact contains no swarm acceptance interface."""

from __future__ import annotations

import argparse
import json
import mmap
import subprocess
from pathlib import Path


def _cache_value(cache_path: Path, key: str) -> str:
    prefix = f"{key}:"
    for line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(prefix):
            return line.split("=", 1)[-1]
    return ""


def _count_marker(binary: mmap.mmap, marker: bytes) -> int:
    count = 0
    offset = 0
    while True:
        offset = binary.find(marker, offset)
        if offset < 0:
            return count
        count += 1
        offset += len(marker)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    build_dir = Path(args.build_dir).resolve()
    cache_path = build_dir / "CMakeCache.txt"
    binary_candidates = [
        build_dir / "Release" / "QGroundControl",
        build_dir / "QGroundControl",
    ]
    binary_path = next((path for path in binary_candidates if path.is_file()), None)
    if not cache_path.is_file() or binary_path is None:
        parser.error(f"release build is incomplete under {build_dir}")

    forbidden_markers = [
        b"QGC_SWARM_ACCEPTANCE_TOKEN_FILE",
        b"QGC_SWARM_ACCEPTANCE_HEADLESS",
        b"QGC_SWARM_ACCEPTANCE_PORT",
        b"SwarmAcceptanceServer",
    ]
    with binary_path.open("rb") as binary_file:  # noqa: SIM117 -- Python 3.8
        with mmap.mmap(binary_file.fileno(), 0, access=mmap.ACCESS_READ) as binary:
            marker_hits = {
                marker.decode("ascii"): _count_marker(binary, marker)
                for marker in forbidden_markers
            }
    symbol_hits = 0
    try:
        nm_process = subprocess.Popen(
            ["nm", "-C", str(binary_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        if nm_process.stdout:
            symbol_hits = sum(line.count("SwarmAcceptanceServer") for line in nm_process.stdout)
        nm_process.wait(timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        if "nm_process" in locals():
            nm_process.kill()
            nm_process.wait()
    testing_enabled = _cache_value(cache_path, "QGC_BUILD_TESTING")
    build_type = _cache_value(cache_path, "CMAKE_BUILD_TYPE")
    passed = (
        testing_enabled == "OFF"
        and build_type == "Release"
        and not any(marker_hits.values())
        and symbol_hits == 0
    )
    payload = {
        "version": 1,
        "binary": str(binary_path),
        "binary_size_bytes": binary_path.stat().st_size,
        "cmake_build_type": build_type,
        "qgc_build_testing": testing_enabled,
        "forbidden_marker_hits": marker_hits,
        "acceptance_symbol_hits": symbol_hits,
        "pass": passed,
    }
    output_path = Path(args.out).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(
        f"{'PASS' if passed else 'FAIL'} release acceptance audit: "
        f"testing={testing_enabled} markers={sum(marker_hits.values())} symbols={symbol_hits}"
    )
    print(f"report={output_path}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
