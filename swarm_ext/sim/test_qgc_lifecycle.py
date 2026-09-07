#!/usr/bin/env python3
"""Exercise managed headless QGC startup, secret hygiene, and signal shutdown."""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qgc_acceptance import (
    QGCAcceptanceClient,
    acceptance_secret_file,
    read_acceptance_token,
)


def _unused_loopback_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def _wait_for_exit(process: subprocess.Popen, timeout_sec: float) -> int:
    try:
        return process.wait(timeout=timeout_sec)
    except subprocess.TimeoutExpired:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=5)
        raise TimeoutError(f"QGC launcher did not exit within {timeout_sec:.0f}s") from None


def _cleanup(process: subprocess.Popen | None, container_name: str) -> None:
    subprocess.run(
        ["docker", "stop", "--time", "3", container_name],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if process and process.poll() is None:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGTERM)
        with contextlib.suppress(subprocess.TimeoutExpired):
            process.wait(timeout=5)
    if process and process.poll() is None:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=5)


def _token_hits_in_process_arguments(token: bytes) -> int:
    hits = 0
    for command_line in Path("/proc").glob("[0-9]*/cmdline"):
        try:
            hits += command_line.read_bytes().count(token)
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
    return hits


def _container_exists(container_name: str) -> bool:
    result = subprocess.run(
        ["docker", "container", "inspect", container_name],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


def _port_is_open(port: int) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.25):
            return True
    except OSError:
        return False


def _run_iteration(
    *,
    build_dir: Path,
    output_dir: Path,
    token_file: Path,
    token: str,
    shutdown_action: str,
    iteration: int,
    port: int,
    wait_until_ready: bool = True,
) -> dict:
    phase = "ready" if wait_until_ready else "startup"
    suffix = f"{phase}-{shutdown_action.lower()}-{iteration}"
    container_name = f"qgc-swarm-lifecycle-{os.getpid()}-{suffix}"
    home_dir = output_dir / f"home-{suffix}"
    log_path = output_dir / f"qgc-{suffix}.log"
    environment = os.environ.copy()
    environment.update(
        {
            "QGC_ACCEPTANCE_CONTAINER_NAME": container_name,
            "QGC_SWARM_ACCEPTANCE_PORT": str(port),
            "QGC_SWARM_ACCEPTANCE_TOKEN_FILE": str(token_file),
        }
    )
    process = None
    started = time.monotonic()
    try:
        with log_path.open("w", encoding="utf-8") as log_file:
            process = subprocess.Popen(
                [
                    str(Path(__file__).with_name("run_qgc_acceptance.sh")),
                    str(build_dir),
                    str(home_dir),
                ],
                stdout=log_file,
                stderr=subprocess.STDOUT,
                env=environment,
                start_new_session=True,
            )
        client = QGCAcceptanceClient("127.0.0.1", port, token, timeout_sec=1.0)
        if wait_until_ready:
            status = client.wait_status(
                lambda response: response.get("vehicle_count") == 0, 60.0, 0.25
            )
            if not status.get("ok"):
                raise RuntimeError("QGC acceptance service did not become ready")
        else:
            deadline = time.monotonic() + 10.0
            while time.monotonic() < deadline and not _container_exists(container_name):
                if process.poll() is not None:
                    raise RuntimeError("managed QGC exited before startup interruption")
                time.sleep(0.01)
            if not _container_exists(container_name):
                raise RuntimeError("managed QGC container did not start")

        encoded_token = token.encode("utf-8")
        process_argument_hits = _token_hits_in_process_arguments(encoded_token)
        inspect_result = subprocess.run(
            ["docker", "container", "inspect", container_name],
            capture_output=True,
            check=False,
        )
        if inspect_result.returncode != 0:
            raise RuntimeError("managed QGC container disappeared before shutdown")
        inspect_hits = inspect_result.stdout.count(encoded_token) + inspect_result.stderr.count(
            encoded_token
        )
        log_hits = log_path.read_bytes().count(encoded_token)
        if process_argument_hits or inspect_hits or log_hits:
            raise RuntimeError(
                "acceptance token was exposed in process arguments, container metadata, or logs"
            )

        if shutdown_action == "QUIT":
            quit_result = client.request("quit")
            if not quit_result.get("ok"):
                raise RuntimeError("QGC rejected the normal quit request")
        else:
            kill_result = subprocess.run(
                ["docker", "kill", "--signal", shutdown_action, container_name],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )
            if kill_result.returncode != 0:
                raise RuntimeError(f"could not send {shutdown_action} to managed QGC")
        exit_code = _wait_for_exit(process, 30.0)
        log_text = log_path.read_text(encoding="utf-8", errors="replace")
        expected_exit_codes = (
            {0} if wait_until_ready or shutdown_action == "QUIT" else {0, 130, 143}
        )
        if exit_code not in expected_exit_codes:
            raise RuntimeError(f"QGC exited with {exit_code} after {shutdown_action}")
        if "ASSERT failure" in log_text or "Segmentation fault" in log_text:
            raise RuntimeError(f"QGC reported an assertion or crash after {shutdown_action}")

        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline and (
            _container_exists(container_name) or _port_is_open(port)
        ):
            time.sleep(0.1)
        container_left = _container_exists(container_name)
        port_left = _port_is_open(port)
        if container_left or port_left:
            raise RuntimeError("QGC shutdown left a container or loopback listener behind")
        return {
            "action": shutdown_action,
            "phase": phase,
            "iteration": iteration,
            "exit_code": exit_code,
            "duration_sec": round(time.monotonic() - started, 3),
            "process_argument_token_hits": process_argument_hits,
            "container_metadata_token_hits": inspect_hits,
            "log_token_hits": log_hits,
            "container_left": container_left,
            "port_left": port_left,
            "pass": True,
        }
    finally:
        _cleanup(process, container_name)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default=str(Path(__file__).resolve().parents[2] / "build"))
    parser.add_argument(
        "--token-file", default=os.environ.get("QGC_SWARM_ACCEPTANCE_TOKEN_FILE", "")
    )
    parser.add_argument("--actions", default="SIGINT,SIGTERM,QUIT")
    parser.add_argument("--signals", default="", help=argparse.SUPPRESS)
    parser.add_argument("--iterations", type=int, default=1)
    parser.add_argument("--early-interrupt", action="store_true")
    parser.add_argument("--out", default="")
    args = parser.parse_args()
    if not shutil.which("docker"):
        parser.error("docker is required")
    if args.iterations < 1:
        parser.error("--iterations must be positive")
    build_dir = Path(args.build_dir).resolve()
    if not (build_dir / "Debug" / "QGroundControl").is_file():
        parser.error(f"QGC test executable is missing under {build_dir}")
    action_list = args.signals or args.actions
    shutdown_actions = [item.strip().upper() for item in action_list.split(",") if item.strip()]
    if not shutdown_actions or any(
        item not in {"SIGINT", "SIGTERM", "QUIT"} for item in shutdown_actions
    ):
        parser.error("--actions accepts only SIGINT, SIGTERM, and QUIT")

    output_dir = (
        Path(args.out).resolve() if args.out else Path(tempfile.mkdtemp(prefix="qgc-lifecycle-"))
    )
    output_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    try:
        with acceptance_secret_file(args.token_file) as token_file:
            token = read_acceptance_token(token_file)
            for iteration in range(1, args.iterations + 1):
                port = _unused_loopback_port()
                for shutdown_action in shutdown_actions:
                    row = _run_iteration(
                        build_dir=build_dir,
                        output_dir=output_dir,
                        token_file=token_file,
                        token=token,
                        shutdown_action=shutdown_action,
                        iteration=iteration,
                        port=port,
                    )
                    rows.append(row)
                    print(
                        f"PASS {shutdown_action} iteration {iteration}: "
                        f"exit={row['exit_code']} duration={row['duration_sec']}s"
                    )
            if args.early_interrupt:
                for shutdown_action in ("SIGINT", "SIGTERM"):
                    row = _run_iteration(
                        build_dir=build_dir,
                        output_dir=output_dir,
                        token_file=token_file,
                        token=token,
                        shutdown_action=shutdown_action,
                        iteration=1,
                        port=_unused_loopback_port(),
                        wait_until_ready=False,
                    )
                    rows.append(row)
                    print(
                        f"PASS early {shutdown_action}: exit={row['exit_code']} "
                        f"duration={row['duration_sec']}s"
                    )
    except (OSError, RuntimeError, TimeoutError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        (output_dir / "report.json").write_text(
            json.dumps({"version": 1, "results": rows}, indent=2), encoding="utf-8"
        )
    print(f"report={output_dir / 'report.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
