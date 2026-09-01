#!/usr/bin/env python3
# ruff: noqa: RUF001, RUF002, RUF003
"""Strict swarm regression: a fresh PX4 fleet and isolated QGC state per scenario."""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_scenario as rs


def _stop_process(process: subprocess.Popen | None, container_name: str) -> None:
    if process and process.poll() is None:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
    if container_name:
        subprocess.run(
            ["docker", "stop", "-t", "3", container_name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )


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


def _wait_qgc(
    client: rs.QGCAcceptanceClient,
    expected_vehicles: int,
    timeout_sec: float,
    processes: list[subprocess.Popen],
) -> dict:
    deadline = time.monotonic() + timeout_sec
    last_status = {}
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
            if (
                last_status.get("ok")
                and last_status.get("vehicle_count", 0) >= expected_vehicles
                and initial_connect_complete
            ):
                return last_status
        except rs.QGCAcceptanceError:
            pass
        time.sleep(1)
    raise TimeoutError(
        f"QGC 在 {timeout_sec:.0f}s 内未达到 {expected_vehicles} 架在线；末次状态={last_status}"
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


def _summary(rows: list[dict], external_qgc: bool, sitl_backend: str) -> str:
    passed = sum(row["result"] == "PASS" for row in rows)
    lines = [
        "# 严格 SITL/QGC 场景回归",
        "",
        "- 隔离策略：每个场景重启 PX4/Gazebo 机群；托管 QGC 时同时使用独立 HOME",
        f"- SITL 后端：`{sitl_backend}`",
        f"- QGC：{'外部正常 GUI' if external_qgc else '托管 headless（QML 场景会明确失败）'}",
        f"- 场景：{len(rows)} · **PASS {passed} / FAIL {len(rows) - passed}**",
        "",
        "| 场景 | 名称 | 结果 | 耗时(s) | 说明 |",
        "|---|---|---|---|---|",
    ]
    for row in rows:
        mark = "✅ PASS" if row["result"] == "PASS" else "❌ FAIL"
        detail = str(row.get("error", "")).replace("|", "\\|").replace("\n", " ")
        lines.append(f"| {row['stem']} | {row['name']} | {mark} | {row['total_sec']} | {detail} |")
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
    parser.add_argument("--qgc-token", default=os.environ.get("QGC_SWARM_ACCEPTANCE_TOKEN", ""))
    parser.add_argument(
        "--external-qgc",
        action="store_true",
        help="使用已启动的正常 GUI QGC；requires_qml 场景必须使用此模式",
    )
    parser.add_argument("--out", default="/tmp/sitl_strict_regression")
    parser.add_argument("--only", default="", help="逗号分隔场景号，例如 01,02,06")
    parser.add_argument("--ready-timeout", type=float, default=2400.0)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    sim_dir = Path(__file__).resolve().parent
    output_dir = Path(args.out).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    scenarios = sorted((sim_dir / "scenarios").glob("*.json"))
    if args.only:
        keys = [key.strip() for key in args.only.split(",") if key.strip()]
        scenarios = [path for path in scenarios if any(path.name.startswith(key) for key in keys)]
    if not scenarios:
        parser.error("没有匹配的场景")
    if (
        not args.dry_run
        and any(
            json.loads(path.read_text(encoding="utf-8")).get("requires_qgc") for path in scenarios
        )
        and len(args.qgc_token) < 16
    ):
        parser.error("QGC 场景要求 --qgc-token 至少 16 个字符")

    rows = []
    for index, scenario_path in enumerate(scenarios, start=1):
        scenario = json.loads(scenario_path.read_text(encoding="utf-8"))
        stem = scenario_path.stem
        started = time.monotonic()
        qgc_process = None
        sitl_process = None
        qgc_container = ""
        sitl_container = ""
        error = ""
        report = {}
        environment_config = {}
        print(f"\n===== 严格场景 {stem}: {scenario['name']} =====", flush=True)
        try:
            if args.dry_run:
                report = rs.run_scenario(scenario, "udpin:0.0.0.0:14550", True)
            else:
                requires_qgc = bool(scenario.get("requires_qgc"))
                if scenario.get("requires_qml") and not args.external_qgc:
                    raise RuntimeError(
                        "该场景要求真实 QML 渲染，请用 --external-qgc 连接正常 GUI QGC"
                    )

                qgc_client = None
                if requires_qgc:
                    if not args.external_qgc:
                        qgc_container = f"qgc-swarm-qgc-{os.getpid()}-{index}"
                        qgc_home = output_dir / f"qgc-home-{stem}-{time.time_ns()}"
                        qgc_env = os.environ.copy()
                        qgc_env.update(
                            {
                                "QGC_ACCEPTANCE_CONTAINER_NAME": qgc_container,
                                "QGC_SWARM_ACCEPTANCE_PORT": str(args.qgc_port),
                                "QGC_SWARM_ACCEPTANCE_TOKEN": args.qgc_token,
                            }
                        )
                        qgc_process = _start_logged(
                            [
                                str(sim_dir / "run_qgc_acceptance.sh"),
                                args.qgc_build,
                                str(qgc_home),
                            ],
                            output_dir / f"qgc_{stem}.log",
                            qgc_env,
                        )
                    qgc_client = rs.QGCAcceptanceClient(
                        args.qgc_host, args.qgc_port, args.qgc_token
                    )
                    _wait_qgc(qgc_client, 0, 60, [p for p in [qgc_process] if p])

                fleet_size = int(scenario.get("fleet", 8))
                if fleet_size > 0:
                    sitl_env = os.environ.copy()
                    if args.sitl_backend == "official":
                        sitl_container = f"qgc-swarm-sitl-{os.getpid()}-{index}"
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
                                "WORK_DIR": str(output_dir / f"px4-work-{stem}-{time.time_ns()}"),
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
                        output_dir / f"sitl_{stem}.log",
                        sitl_env,
                    )
                    if qgc_client:
                        _wait_qgc(
                            qgc_client,
                            fleet_size,
                            args.ready_timeout,
                            [p for p in [qgc_process, sitl_process] if p],
                        )
                        if args.sitl_backend == "local" and not args.external_qgc:
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
        except Exception as exc:
            error = str(exc)
            report = {
                "scenario": scenario["name"],
                "pass": False,
                "error": error,
                "steps": report.get("steps", []),
            }
        finally:
            _stop_process(sitl_process, sitl_container)
            if not args.external_qgc:
                _stop_process(qgc_process, qgc_container)

        report["fresh_fleet"] = not args.dry_run
        if environment_config:
            report["environment_config"] = environment_config
        report["total_sec"] = round(time.monotonic() - started, 1)
        (output_dir / f"report_{stem}.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        rows.append(
            {
                "stem": stem,
                "name": scenario["name"],
                "result": "PASS" if report.get("pass") else "FAIL",
                "total_sec": report["total_sec"],
                "error": error,
            }
        )

    summary = _summary(rows, args.external_qgc, args.sitl_backend)
    (output_dir / "SUMMARY.md").write_text(summary, encoding="utf-8")
    print("\n" + summary)
    raise SystemExit(0 if all(row["result"] == "PASS" for row in rows) else 1)


if __name__ == "__main__":
    main()
