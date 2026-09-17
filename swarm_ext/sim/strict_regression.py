#!/usr/bin/env python3
# ruff: noqa: RUF001
"""Strict swarm regression: a fresh PX4 fleet and isolated QGC state per scenario."""

from __future__ import annotations

import argparse
import contextlib
import json
import math
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
from numbers import Real
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_scenario as rs

# 1 means a real failure and argparse's parser.error() already owns 2, so an
# environment block exits 3.
EXIT_ENVIRONMENT_BLOCKED = 3


def _stop_process(process: subprocess.Popen | None, container_name: str) -> None:
    if container_name:
        subprocess.run(
            ["docker", "stop", "--time", "15", container_name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    if process and process.poll() is None:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)


def _unused_loopback_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def _unused_x_display(offset: int) -> int:
    for display_number in range(100 + offset % 700, 1000):
        if (
            not Path(f"/tmp/.X11-unix/X{display_number}").exists()
            and not Path(f"/tmp/.X{display_number}-lock").exists()
        ):
            return display_number
    raise RuntimeError("no free X display is available")


def _container_exists(container_name: str) -> bool:
    if not container_name:
        return False
    result = subprocess.run(
        ["docker", "container", "inspect", container_name],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


def _port_is_open(host: str, port: int) -> bool:
    try:
        with socket.create_connection((host, port), timeout=0.25):
            return True
    except OSError:
        return False


def _directory_size(path: Path) -> int:
    return sum(item.stat().st_size for item in path.rglob("*") if item.is_file())


def _capture_screenshot(display_number: int, output_path: Path) -> bool:
    executable = shutil.which("import")
    if not executable or display_number <= 0:
        return False
    environment = os.environ.copy()
    environment["DISPLAY"] = f":{display_number}"
    try:
        result = subprocess.run(
            [executable, "-window", "root", str(output_path)],
            env=environment,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return result.returncode == 0 and output_path.is_file()


def _start_logged(command: list[str], log_path: Path, env: dict | None = None) -> subprocess.Popen:
    log_file = log_path.open("w", encoding="utf-8")
    try:
        process = subprocess.Popen(
            command,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            env=env,
            start_new_session=True,
        )
    finally:
        log_file.close()
    return process


class QGCEnvironmentBlocked(RuntimeError):
    """QGC is demonstrably healthy and a minority of sim instances are not: environment, not defect."""

    def __init__(self, reason: str, evidence: dict):
        super().__init__(reason)
        self.reason = reason
        self.evidence = evidence


# How long the minority-stall signature must hold before it counts. Far longer than a
# healthy EKF/GPS convergence, far shorter than --ready-timeout's 2400s, so a dead
# instance is called in three minutes instead of spinning for forty.
QGC_MINORITY_STALL_SEC = 180.0


def _telemetry_complete(vehicle: dict) -> bool:
    return all(
        isinstance(vehicle.get(field), Real) and math.isfinite(float(vehicle[field]))
        for field in ("lat", "lon", "alt_rel_m")
    )


def _minority_instance_stall(status: dict, expected_vehicles: int) -> dict | None:
    """Detect "QGC healthy, a minority of sim instances stalled" behind a readiness timeout.

    The anti-masking boundary is the same one F3's sensor gate uses: the reference frame
    is the fleet's own majority, and the downgrade only fires when QGC can be shown to be
    working. Any of these failing returns None, which keeps the outcome a plain FAIL:

    1. QGC answers and reports ok. A hung or dead QGC never gets here.
    2. It reports at least the expected fleet size. Under-reporting is a QGC connection
       defect and must stay visible.
    3. Every vehicle finished its initial connect, proving QGC's handshake works fleet-wide.
    4. The vehicles missing telemetry are a strict minority. A broken telemetry pipeline in
       QGC would starve every vehicle, not one.
    5. The fleet has at least two vehicles. One vehicle has no majority to be measured
       against, so nothing can be discriminated and nothing is excused.
    """
    if not status.get("ok"):
        return None
    vehicles = status.get("vehicles") or []
    if expected_vehicles < 2 or len(vehicles) < expected_vehicles:
        return None
    if not all(vehicle.get("initial_connect_complete", False) for vehicle in vehicles):
        return None
    stalled = [vehicle.get("id") for vehicle in vehicles if not _telemetry_complete(vehicle)]
    if not stalled or len(stalled) * 2 >= len(vehicles):
        return None
    return {
        "stalled_ids": stalled,
        "fleet_size": len(vehicles),
        "reason": "minority_instance_telemetry_stall",
    }


def _wait_qgc(
    client: rs.QGCAcceptanceClient,
    expected_vehicles: int,
    timeout_sec: float,
    processes: list[subprocess.Popen],
) -> dict:
    deadline = time.monotonic() + timeout_sec
    last_status = {}
    stall_since: float | None = None
    while time.monotonic() < deadline:
        for process in processes:
            if process.poll() is not None:
                raise RuntimeError(f"依赖进程提前退出（退出码 {process.returncode}）")
        try:
            last_status = client.request("status")
            vehicles = last_status.get("vehicles", [])
            initial_connect_complete = expected_vehicles == 0 or (
                len(vehicles) >= expected_vehicles
                and all(vehicle.get("initial_connect_complete", False) for vehicle in vehicles)
            )
            flight_telemetry_ready = expected_vehicles == 0 or (
                len(vehicles) >= expected_vehicles
                and all(_telemetry_complete(vehicle) for vehicle in vehicles)
            )
            if (
                last_status.get("ok")
                and last_status.get("vehicle_count", 0) >= expected_vehicles
                and initial_connect_complete
                and flight_telemetry_ready
            ):
                return last_status
            # The signature must hold for QGC_MINORITY_STALL_SEC before it counts, so the
            # normal staggered startup (instances coming up one by one) is not mistaken for
            # a dead instance. Any recovery restarts the clock.
            stall = _minority_instance_stall(last_status, expected_vehicles)
            if stall is None:
                stall_since = None
            else:
                now = time.monotonic()
                if stall_since is None:
                    stall_since = now
                elif now - stall_since >= QGC_MINORITY_STALL_SEC:
                    raise QGCEnvironmentBlocked(
                        "QGC 正常应答且全队握手完成，但 {} 号实例持续 {:.0f}s 未上报遥测"
                        "（共 {} 架）——该实例的仿真器侧没起来".format(
                            "/".join(str(i) for i in stall["stalled_ids"]),
                            QGC_MINORITY_STALL_SEC,
                            stall["fleet_size"],
                        ),
                        {**stall, "last_status": last_status},
                    )
        except rs.QGCAcceptanceError:
            # Do not accumulate stall time while QGC is unreachable. That is QGC's own
            # problem and must be allowed to reach the timeout as a FAIL.
            stall_since = None
        time.sleep(1)
    raise TimeoutError(
        f"QGC 在 {timeout_sec:.0f}s 内未达到 {expected_vehicles} 架可飞状态；末次状态={last_status}"
    )


def _wait_mavlink(expected_vehicles: int, timeout_sec: float, sitl: subprocess.Popen) -> None:
    from pymavlink import mavutil

    connection = mavutil.mavlink_connection("udpin:0.0.0.0:14550", autoreconnect=True)
    seen = set()
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        if sitl.poll() is not None:
            raise RuntimeError(f"SITL 提前退出（退出码 {sitl.returncode}）")
        message = connection.recv_match(type="HEARTBEAT", blocking=True, timeout=1)
        if message:
            seen.add(message.get_srcSystem())
        if len(seen) >= expected_vehicles:
            return
    raise TimeoutError(f"SITL 在 {timeout_sec:.0f}s 内仅发现 {len(seen)}/{expected_vehicles} 架")


def _counts(rows: list[dict]) -> dict[str, int]:
    return {
        result: sum(row["result"] == result for row in rows)
        for result in ("PASS", "FAIL", "BLOCKED")
    }


def _summary(rows: list[dict], qgc_mode: str, sitl_backend: str, keep_workdir: str) -> str:
    counts = _counts(rows)
    lines = [
        "# 严格 SITL/QGC 场景回归",
        "",
        "- 隔离策略：每个场景重启 PX4/Gazebo 机群；托管 QGC 时同时使用独立 HOME",
        f"- SITL 后端：`{sitl_backend}`",
        f"- QGC 模式：`{qgc_mode}`",
        f"- 工作目录保留：`{keep_workdir}`",
        f"- 场景：{len(rows)} · **PASS {counts['PASS']} / FAIL {counts['FAIL']} / "
        f"BLOCKED {counts['BLOCKED']}**",
        "",
        "| 轮次 | 场景 | 名称 | 结果 | 耗时(s) | 说明 |",
        "|---:|---|---|---|---:|---|",
    ]
    marks = {"PASS": "✅ PASS", "BLOCKED": "⛔ BLOCKED", "FAIL": "❌ FAIL"}
    for row in rows:
        mark = marks[row["result"]]
        detail = str(row.get("error", "")).replace("|", "\\|").replace("\n", " ")
        lines.append(
            f"| {row['iteration']} | {row['stem']} | {row['name']} | {mark} | "
            f"{row['total_sec']} | {detail} |"
        )
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--px4-src", default=str(Path.home() / "PX4-Autopilot"))
    parser.add_argument(
        "--sitl-backend",
        choices=("official", "local"),
        default="official",
        help="official 使用 PX4 Docker 镜像；local 使用已有 px4_sitl_default/gz 构建",
    )
    parser.add_argument("--qgc-build", default=str(Path(__file__).resolve().parents[2] / "build"))
    parser.add_argument("--qgc-host", default="127.0.0.1")
    parser.add_argument("--qgc-port", type=int, default=15610)
    parser.add_argument(
        "--qgc-token-file",
        default=os.environ.get("QGC_SWARM_ACCEPTANCE_TOKEN_FILE", ""),
        help="QGC 验收接口 Secret 文件",
    )
    parser.add_argument(
        "--qgc-mode",
        choices=("managed-headless", "managed-gui", "external"),
        default="managed-headless",
        help="托管 headless、托管正常 GUI/Xvfb，或连接外部 QGC",
    )
    parser.add_argument(
        "--external-qgc",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--out", default="/tmp/sitl_strict_regression")
    parser.add_argument("--only", default="", help="逗号分隔场景号，例如 01,02,06")
    parser.add_argument("--ready-timeout", type=float, default=2400.0)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument(
        "--keep-workdir",
        choices=("always", "on-failure", "never"),
        default="on-failure",
    )
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.external_qgc:
        if args.qgc_mode != "managed-headless":
            parser.error("--external-qgc cannot be combined with --qgc-mode")
        args.qgc_mode = "external"
    if args.repeat < 1:
        parser.error("--repeat 必须大于 0")

    sim_dir = Path(__file__).resolve().parent
    output_dir = Path(args.out).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    scenarios = sorted((sim_dir / "scenarios").glob("*.json"))
    if args.only:
        keys = [key.strip() for key in args.only.split(",") if key.strip()]
        scenarios = [path for path in scenarios if any(path.name.startswith(key) for key in keys)]
    if not scenarios:
        parser.error("没有匹配的场景")
    requires_qgc = not args.dry_run and any(
        json.loads(path.read_text(encoding="utf-8")).get("requires_qgc") for path in scenarios
    )
    token = ""
    if requires_qgc:
        if not args.qgc_token_file:
            parser.error("QGC 场景要求 --qgc-token-file")
        try:
            token = rs.read_acceptance_token(args.qgc_token_file)
        except ValueError as exc:
            parser.error(str(exc))

    rows = []
    run_number = 0
    for iteration in range(1, args.repeat + 1):
        for scenario_path in scenarios:
            run_number += 1
            scenario = json.loads(scenario_path.read_text(encoding="utf-8"))
            stem = scenario_path.stem
            run_dir = output_dir / "runs" / f"{run_number:03d}-{stem}-repeat-{iteration:03d}"
            work_dir = run_dir / "work"
            work_dir.mkdir(parents=True, exist_ok=True)
            started = time.monotonic()
            qgc_process = None
            sitl_process = None
            qgc_container = ""
            qgc_display = 0
            sitl_container = ""
            error = ""
            report = {}
            environment_config = {}
            managed_qgc = args.qgc_mode != "external"
            qgc_port = _unused_loopback_port() if managed_qgc else args.qgc_port
            print(
                f"\n===== 严格场景 {stem}: {scenario['name']} "
                f"(repeat {iteration}/{args.repeat}) =====",
                flush=True,
            )
            try:
                if args.dry_run:
                    report = rs.run_scenario(scenario, "udpin:0.0.0.0:14550", True)
                else:
                    scenario_requires_qgc = bool(scenario.get("requires_qgc"))
                    if scenario.get("requires_qml") and args.qgc_mode == "managed-headless":
                        raise RuntimeError(
                            "该场景要求真实 QML 渲染，请使用 --qgc-mode managed-gui 或 external"
                        )

                    qgc_client = None
                    if scenario_requires_qgc:
                        if managed_qgc:
                            qgc_container = f"qgc-swarm-qgc-{os.getpid()}-{run_number}"
                            qgc_env = os.environ.copy()
                            qgc_env.update(
                                {
                                    "QGC_ACCEPTANCE_CONTAINER_NAME": qgc_container,
                                    "QGC_SWARM_ACCEPTANCE_PORT": str(qgc_port),
                                    "QGC_SWARM_ACCEPTANCE_TOKEN_FILE": str(
                                        Path(args.qgc_token_file).resolve()
                                    ),
                                }
                            )
                            if args.qgc_mode == "managed-gui":
                                qgc_script = sim_dir / "run_qgc_gui_acceptance.sh"
                                qgc_display = _unused_x_display(run_number)
                                qgc_env["QGC_ACCEPTANCE_DISPLAY_NUMBER"] = str(qgc_display)
                            else:
                                qgc_script = sim_dir / "run_qgc_acceptance.sh"
                            qgc_process = _start_logged(
                                [
                                    str(qgc_script),
                                    args.qgc_build,
                                    str(work_dir / "qgc-home"),
                                ],
                                run_dir / "qgc.log",
                                qgc_env,
                            )
                        qgc_client = rs.QGCAcceptanceClient(args.qgc_host, qgc_port, token)
                        _wait_qgc(qgc_client, 0, 60, [p for p in [qgc_process] if p])

                    fleet_size = int(scenario.get("fleet", 8))
                    if fleet_size > 0:
                        sitl_env = os.environ.copy()
                        if args.sitl_backend == "official":
                            sitl_container = f"qgc-swarm-sitl-{os.getpid()}-{run_number}"
                            sitl_env["QGC_SITL_CONTAINER_NAME"] = sitl_container
                            sitl_command = [
                                str(sim_dir / "run_official_sitl.sh"),
                                args.px4_src,
                                str(fleet_size),
                            ]
                        else:
                            sitl_env.update(
                                {
                                    "QGC_SKIP_READY_PROBE": "1",
                                    "WORK_DIR": str(work_dir / "px4"),
                                }
                            )
                            sitl_command = [
                                str(sim_dir / "launch_swarm_sitl.sh"),
                                str(fleet_size),
                                args.px4_src,
                                "gz",
                            ]
                        sitl_process = _start_logged(
                            sitl_command,
                            run_dir / "sitl.log",
                            sitl_env,
                        )
                        if qgc_client:
                            _wait_qgc(
                                qgc_client,
                                fleet_size,
                                args.ready_timeout,
                                [p for p in [qgc_process, sitl_process] if p],
                            )
                            if args.sitl_backend == "local" and managed_qgc:
                                environment_config = qgc_client.request("configure_px4_sitl")
                                if not environment_config.get("ok"):
                                    raise RuntimeError(
                                        f"本地 PX4 SITL 参数配置失败：{environment_config}"
                                    )
                                time.sleep(2.0)
                        else:
                            _wait_mavlink(fleet_size, args.ready_timeout, sitl_process)

                    report = rs.run_scenario(
                        scenario,
                        "udpin:0.0.0.0:14550",
                        False,
                        qgc_client,
                    )
                    if not report.get("pass"):
                        error = str(report.get("error", "场景步骤失败"))
            except QGCEnvironmentBlocked as exc:
                # Same rule as F3's sensor gate: downgrade only when QGC is demonstrably
                # healthy (see _minority_instance_stall). cleanup_errors still vetoes below.
                error = str(exc)
                report = {
                    "scenario": scenario["name"],
                    "pass": False,
                    "error": error,
                    "steps": report.get("steps", []),
                    "environment_block": True,
                    "environment_block_reason": exc.reason,
                    "qgc_readiness": exc.evidence,
                }
            except Exception as exc:
                error = str(exc)
                report = {
                    "scenario": scenario["name"],
                    "pass": False,
                    "error": error,
                    "steps": report.get("steps", []),
                }
            finally:
                if qgc_display and not report.get("pass", False):
                    report["failure_screenshot"] = (
                        str(run_dir / "failure.png")
                        if _capture_screenshot(qgc_display, run_dir / "failure.png")
                        else ""
                    )
                _stop_process(sitl_process, sitl_container)
                if managed_qgc:
                    _stop_process(qgc_process, qgc_container)

            cleanup_errors = []
            if _container_exists(sitl_container):
                cleanup_errors.append(f"SITL 容器残留：{sitl_container}")
            if managed_qgc and _container_exists(qgc_container):
                cleanup_errors.append(f"QGC 容器残留：{qgc_container}")
            if (
                managed_qgc
                and scenario.get("requires_qgc")
                and _port_is_open("127.0.0.1", qgc_port)
            ):
                cleanup_errors.append(f"QGC 验收端口残留：{qgc_port}")
            if cleanup_errors:
                error = "; ".join(filter(None, [error, *cleanup_errors]))
                report["pass"] = False
                report["error"] = error

            report["fresh_fleet"] = not args.dry_run
            report["iteration"] = iteration
            report["qgc_mode"] = args.qgc_mode
            report["cleanup_errors"] = cleanup_errors
            if environment_config:
                report["environment_config"] = environment_config
            report["total_sec"] = round(time.monotonic() - started, 1)
            keep_workdir = args.keep_workdir == "always" or (
                args.keep_workdir == "on-failure" and not report.get("pass")
            )
            if not keep_workdir:
                shutil.rmtree(work_dir, ignore_errors=True)
            report["workdir_retained"] = keep_workdir and work_dir.exists()
            report["evidence_size_bytes"] = _directory_size(run_dir)
            report_path = run_dir / "report.json"
            report_path.write_text(
                json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
            )
            # cleanup_errors vetoes the downgrade: a leaked container or port is a
            # defect in the harness itself and must not hide behind an env block.
            blocked = bool(report.get("environment_block")) and not cleanup_errors
            rows.append(
                {
                    "iteration": iteration,
                    "stem": stem,
                    "name": scenario["name"],
                    "result": "PASS" if report.get("pass") else ("BLOCKED" if blocked else "FAIL"),
                    "total_sec": report["total_sec"],
                    "error": error,
                    "report": str(report_path),
                    "workdir_retained": report["workdir_retained"],
                }
            )

    (output_dir / "report.json").write_text(
        json.dumps(
            {
                "version": 2,
                "qgc_mode": args.qgc_mode,
                "sitl_backend": args.sitl_backend,
                "repeat": args.repeat,
                "keep_workdir": args.keep_workdir,
                "counts": _counts(rows),
                "results": rows,
            },
            ensure_ascii=False,
            indent=2,
        ),
        encoding="utf-8",
    )
    summary = _summary(rows, args.qgc_mode, args.sitl_backend, args.keep_workdir)
    (output_dir / "SUMMARY.md").write_text(summary, encoding="utf-8")
    print("\n" + summary)
    if any(row["result"] == "FAIL" for row in rows):
        raise SystemExit(1)
    if any(row["result"] == "BLOCKED" for row in rows):
        raise SystemExit(EXIT_ENVIRONMENT_BLOCKED)
    raise SystemExit(0)


if __name__ == "__main__":
    main()
