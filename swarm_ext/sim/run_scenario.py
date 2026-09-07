#!/usr/bin/env python3
# ruff: noqa: RUF001, RUF002, RUF003
"""
run_scenario.py — 集群 SITL 场景运行器（WP1.5）

驱动 scenarios/*.json 场景：通过 MAVLink（pymavlink）连接 PX4 SITL 机群，
按时间线执行动作/故障注入，输出 JSON 报告。
关键实测项：一键暂停（中止）指令→模式确认时延（验收 ≤1s）。

用法：
  python3 run_scenario.py scenarios/05_emergency_stop.json [--report out.json]
  python3 run_scenario.py scenarios/01_area_search.json --dry-run   # 只解析打印时间线
依赖：pip install pymavlink（--dry-run 不需要）
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

from qgc_acceptance import (
    QGCAcceptanceClient,
    QGCAcceptanceError,
    cop_truth_metrics,
    formation_geometry_metrics,
    read_acceptance_token,
)

HEARTBEAT_TIMEOUT_SEC = 5.0  # 超过该时长未收到心跳视为离线


def _finite_float(value, default: float = math.nan) -> float:
    try:
        number = float(value)
    except (TypeError, ValueError):
        return default
    return number if math.isfinite(number) else default


class Fleet:
    """MAVLink 机群连接：单 UDP 入口按 sysid 区分各机（与 QGC 接入机制一致）。"""

    def __init__(self, conn_str: str):
        from pymavlink import mavutil  # 延迟导入，dry-run 无需依赖

        self.mav = mavutil.mavlink_connection(conn_str, autoreconnect=True)
        self.mavutil = mavutil
        self.last_heartbeat = {}  # sysid -> (timestamp, custom_mode, base_mode)
        self.rel_alt = {}  # sysid -> 相对高度（米）
        self.amsl_alt = {}  # sysid -> AMSL 高度（米）
        self.position = {}  # sysid -> (lat, lon)
        self.last_condition_detail = {}

    def remember(self, msg) -> None:
        """更新在线状态和高度缓存。"""
        sid = msg.get_srcSystem()
        mtype = msg.get_type()
        if mtype == "HEARTBEAT":
            self.last_heartbeat[sid] = (time.time(), msg.custom_mode, msg.base_mode)
        elif mtype == "GLOBAL_POSITION_INT":
            self.rel_alt[sid] = msg.relative_alt / 1000.0
            self.amsl_alt[sid] = msg.alt / 1000.0
            self.position[sid] = (msg.lat / 1e7, msg.lon / 1e7)

    def pump(self, duration: float = 0.0) -> None:
        """处理积压消息并（可选）等待 duration 秒。"""
        deadline = time.time() + duration
        while True:
            msg = self.mav.recv_match(blocking=True, timeout=0.2)
            if msg is None:
                if time.time() >= deadline:
                    return
                continue
            self.remember(msg)
            if time.time() >= deadline:
                return

    def online_ids(self) -> list:
        now = time.time()
        return sorted(
            sid for sid, (t, _, _) in self.last_heartbeat.items() if now - t < HEARTBEAT_TIMEOUT_SEC
        )

    def wait_all_online(self, fleet: int, timeout: float) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.pump(1.0)
            if len(self.online_ids()) >= fleet:
                return True
        return False

    def wait_all_airborne(self, min_alt: float, timeout: float, expected_count: int) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.pump(1.0)
            online = self.online_ids()
            self.last_condition_detail = {
                "expected_online": expected_count,
                "online": online,
                "relative_altitudes_m": {
                    sid: round(self.rel_alt.get(sid, -999.0), 2) for sid in online
                },
            }
            if len(online) >= expected_count and all(
                self.rel_alt.get(sid, -999.0) >= min_alt for sid in online
            ):
                return True
        return False

    def wait_all_disarmed_or_low(self, max_alt: float, timeout: float, expected_count: int) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.pump(1.0)
            online = self.online_ids()
            armed = [
                sid
                for sid in online
                if self.last_heartbeat[sid][2] & self.mavutil.mavlink.MAV_MODE_FLAG_SAFETY_ARMED
            ]
            high = [sid for sid in online if self.rel_alt.get(sid, 999.0) > max_alt]
            self.last_condition_detail = {
                "expected_online": expected_count,
                "online": online,
                "armed": armed,
                "above_max_altitude": high,
                "relative_altitudes_m": {
                    sid: round(self.rel_alt.get(sid, 999.0), 2) for sid in online
                },
            }
            if len(online) >= expected_count and not armed and not high:
                return True
        return False

    def send_command(
        self, sysid: int, command: int, p1=0.0, p2=0.0, p3=0.0, p4=0.0, p5=0.0, p6=0.0, p7=0.0
    ) -> None:
        self.mav.mav.command_long_send(sysid, 1, command, 0, p1, p2, p3, p4, p5, p6, p7)

    def broadcast(
        self,
        command: int,
        p1=0.0,
        p2=0.0,
        p3=0.0,
        p4=0.0,
        p5=0.0,
        p6=0.0,
        p7=0.0,
        ack_timeout: float = 5.0,
        relative_altitude: bool = False,
    ) -> dict:
        """逐机发送命令并收齐 ACK；拒绝、超时或空目标均判失败。"""
        targets = self.online_ids()
        missing_position = []
        command_targets = []
        results = {}
        ack_history = {sid: [] for sid in targets}
        for sid in targets:
            target_p7 = p7
            if relative_altitude:
                if sid not in self.amsl_alt:
                    missing_position.append(sid)
                    continue
                target_p7 = self.amsl_alt[sid] + p7
            self.send_command(sid, command, p1, p2, p3, p4, p5, p6, target_p7)
            command_targets.append(sid)

        accepted = {
            self.mavutil.mavlink.MAV_RESULT_ACCEPTED,
            self.mavutil.mavlink.MAV_RESULT_IN_PROGRESS,
        }
        pending = set(command_targets)
        deadline = time.time() + ack_timeout
        while pending and time.time() < deadline:
            msg = self.mav.recv_match(blocking=True, timeout=0.2)
            if msg is None:
                continue
            self.remember(msg)
            if msg.get_type() != "COMMAND_ACK" or msg.command != command:
                continue
            sid = msg.get_srcSystem()
            if sid in ack_history:
                result = int(msg.result)
                ack_history[sid].append(result)
            if sid in pending and result in accepted:
                pending.discard(sid)
                results[sid] = result

        for sid in pending:
            if ack_history[sid]:
                results[sid] = ack_history[sid][-1]
        missing_ack = sorted(sid for sid in pending if not ack_history[sid])
        rejected = {sid: results[sid] for sid in pending if sid in results}
        return {
            "targets": targets,
            "ack_results": results,
            "ack_history": ack_history,
            "missing_ack": missing_ack,
            "missing_position": missing_position,
            "rejected": rejected,
            "pass": bool(targets) and not pending and not missing_position,
        }

    def arm_all(self, retry_timeout: float = 60.0) -> dict:
        """对仿真启动期的临时拒绝做有限重试；硬拒绝不会被掩盖。"""
        m = self.mavutil.mavlink
        deadline = time.time() + retry_timeout
        attempts = []
        while True:
            result = self.broadcast(m.MAV_CMD_COMPONENT_ARM_DISARM, p1=1.0, p2=0.0)
            attempts.append(
                {
                    "elapsed_sec": round(retry_timeout - max(0.0, deadline - time.time()), 1),
                    "ack_results": result["ack_results"],
                    "ack_history": result["ack_history"],
                    "missing_ack": result["missing_ack"],
                    "rejected": result["rejected"],
                }
            )
            if result["pass"]:
                if len(attempts) > 1:
                    # 首轮已解锁的实例可能在等待其他实例预检时触发 PX4 地面自动上锁。
                    # 全体预检通过后在地面同步一次上锁/解锁，确保起飞窗口一致。
                    disarm = self.broadcast(m.MAV_CMD_COMPONENT_ARM_DISARM, p1=0.0, p2=0.0)
                    result["resync_disarm"] = disarm
                    if not disarm["pass"]:
                        result["pass"] = False
                        result["attempts"] = attempts
                        return result
                    self.pump(1.0)
                    result = self.broadcast(m.MAV_CMD_COMPONENT_ARM_DISARM, p1=1.0, p2=0.0)
                    result["resync_disarm"] = disarm
                result["attempts"] = attempts
                return result

            hard_rejected = {
                sid: code
                for sid, code in result["rejected"].items()
                if code != m.MAV_RESULT_TEMPORARILY_REJECTED
            }
            if hard_rejected or time.time() >= deadline:
                result["attempts"] = attempts
                return result
            self.pump(min(3.0, max(0.0, deadline - time.time())))

    def measure_pause_latency(self, threshold_ms: int) -> dict:
        """对全体下发暂停（DO_REPOSITION 悬停），测各机 COMMAND_ACK 时延。"""
        m = self.mavutil.mavlink
        targets = self.online_ids()
        t0 = time.time()
        pending = set(targets)
        ack_history = {sid: [] for sid in targets}
        for sid in targets:
            self.mav.mav.command_long_send(
                sid,
                1,
                m.MAV_CMD_DO_REPOSITION,
                0,
                -1,
                m.MAV_DO_REPOSITION_FLAGS_CHANGE_MODE,
                0,
                float("nan"),
                float("nan"),
                float("nan"),
                float("nan"),
            )
        latencies = {}
        deadline = t0 + threshold_ms / 1000.0 * 3
        while pending and time.time() < deadline:
            msg = self.mav.recv_match(type="COMMAND_ACK", blocking=True, timeout=0.2)
            if msg is None:
                continue
            sid = msg.get_srcSystem()
            if sid in ack_history and msg.command == m.MAV_CMD_DO_REPOSITION:
                ack_history[sid].append(int(msg.result))
            if (
                sid in pending
                and msg.command == m.MAV_CMD_DO_REPOSITION
                and msg.result in (m.MAV_RESULT_ACCEPTED, m.MAV_RESULT_IN_PROGRESS)
            ):
                pending.discard(sid)
                latencies[sid] = round((time.time() - t0) * 1000, 1)
        for sid in pending:
            latencies[sid] = None  # 超时未确认
        values = [v for v in latencies.values() if v is not None]
        return {
            "per_vehicle_ms": latencies,
            "ack_history": ack_history,
            "max_ms": max(values) if values else None,
            "pass": bool(values) and max(values) <= threshold_ms and not pending,
            "threshold_ms": threshold_ms,
        }

    def degrade_mavlink(
        self, instance: int, seconds: float, expected_count: int, recovery_timeout: float
    ) -> dict:
        """仅丢弃一架机发往 GCS 的 MAVLink，避免暂停共享仿真时钟。"""
        source_port = 18570 + instance
        target_sysid = instance + 1
        tag = f"qgc-sitl-link-{os.getpid()}-{instance}"
        rule = [
            "OUTPUT",
            "-p",
            "udp",
            "--sport",
            str(source_port),
            "--dport",
            "14550",
            "-m",
            "comment",
            "--comment",
            tag,
            "-j",
            "DROP",
        ]
        add = subprocess.run(
            ["sudo", "-n", "iptables", "-I", *rule], capture_output=True, text=True
        )
        if add.returncode != 0:
            return {
                "pass": False,
                "instance": instance,
                "target_sysid": target_sysid,
                "source_port": source_port,
                "error": (add.stderr or add.stdout).strip(),
            }

        observed_offline = False
        during_online = []
        remove_error = ""
        try:
            deadline = time.time() + seconds
            while time.time() < deadline:
                self.pump(min(1.0, max(0.0, deadline - time.time())))
                during_online = self.online_ids()
                if target_sysid not in during_online and len(during_online) >= expected_count - 1:
                    observed_offline = True
        finally:
            remove = subprocess.run(
                ["sudo", "-n", "iptables", "-D", *rule], capture_output=True, text=True
            )
            if remove.returncode != 0:
                remove_error = (remove.stderr or remove.stdout).strip()

        recovered = False
        recovery_online = []
        deadline = time.time() + recovery_timeout
        while time.time() < deadline:
            self.pump(min(1.0, max(0.0, deadline - time.time())))
            recovery_online = self.online_ids()
            if target_sysid in recovery_online and len(recovery_online) >= expected_count:
                recovered = True
                break

        return {
            "pass": observed_offline and recovered and not remove_error,
            "instance": instance,
            "target_sysid": target_sysid,
            "source_port": source_port,
            "seconds": seconds,
            "observed_offline": observed_offline,
            "online_during_drop": during_online,
            "recovered": recovered,
            "online_after_recovery": recovery_online,
            "remove_error": remove_error,
        }


class QGCFleet:
    """Fleet telemetry/control facade backed exclusively by QGC's acceptance interface."""

    def __init__(self, client: QGCAcceptanceClient):
        self.client = client
        self.position = {}
        self.rel_alt = {}
        self.amsl_alt = {}
        self.last_condition_detail = {}
        self._status = {}
        self._expected_count = 0

    def _refresh(self) -> dict:
        self._status = self.client.request("status")
        for vehicle in self._status.get("vehicles", []):
            vehicle_id = int(vehicle["id"])
            latitude = _finite_float(vehicle.get("lat"))
            longitude = _finite_float(vehicle.get("lon"))
            relative_altitude = _finite_float(vehicle.get("alt_rel_m"))
            amsl_altitude = _finite_float(vehicle.get("alt_amsl_m"))
            if math.isfinite(latitude) and math.isfinite(longitude):
                self.position[vehicle_id] = (latitude, longitude)
            if math.isfinite(relative_altitude):
                self.rel_alt[vehicle_id] = relative_altitude
            if math.isfinite(amsl_altitude):
                self.amsl_alt[vehicle_id] = amsl_altitude
        return self._status

    def pump(self, duration: float = 0.0) -> None:
        deadline = time.monotonic() + duration
        while True:
            self._refresh()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            time.sleep(min(0.5, remaining))

    def online_ids(self) -> list[int]:
        return sorted(int(vehicle["id"]) for vehicle in self._status.get("vehicles", []))

    def wait_all_online(self, fleet: int, timeout: float) -> bool:
        self._expected_count = fleet
        status = self.client.wait_status(
            lambda current: current.get("vehicle_count", 0) >= fleet, timeout
        )
        self._status = status
        self.pump()
        return len(self.online_ids()) >= fleet

    def wait_all_airborne(self, min_alt: float, timeout: float, expected_count: int) -> bool:
        status = self.client.wait_status(
            lambda current: len(current.get("vehicles", [])) >= expected_count
            and all(
                _finite_float(vehicle.get("alt_rel_m"), -999.0) >= min_alt
                for vehicle in current.get("vehicles", [])
            ),
            timeout,
        )
        self._status = status
        self.pump()
        online = self.online_ids()
        self.last_condition_detail = {
            "expected_online": expected_count,
            "online": online,
            "relative_altitudes_m": {
                vehicle_id: round(self.rel_alt.get(vehicle_id, -999.0), 2) for vehicle_id in online
            },
        }
        return len(online) >= expected_count and all(
            self.rel_alt.get(vehicle_id, -999.0) >= min_alt for vehicle_id in online
        )

    def wait_all_disarmed_or_low(self, max_alt: float, timeout: float, expected_count: int) -> bool:
        status = self.client.wait_status(
            lambda current: len(current.get("vehicles", [])) >= expected_count
            and all(
                not vehicle.get("armed")
                and _finite_float(vehicle.get("alt_rel_m"), 999.0) <= max_alt
                for vehicle in current.get("vehicles", [])
            ),
            timeout,
        )
        self._status = status
        self.pump()
        vehicles = self._status.get("vehicles", [])
        armed = [int(vehicle["id"]) for vehicle in vehicles if vehicle.get("armed")]
        high = [
            int(vehicle["id"])
            for vehicle in vehicles
            if _finite_float(vehicle.get("alt_rel_m"), 999.0) > max_alt
        ]
        self.last_condition_detail = {
            "expected_online": expected_count,
            "online": self.online_ids(),
            "armed": armed,
            "above_max_altitude": high,
        }
        return len(vehicles) >= expected_count and not armed and not high

    def arm_all(self, retry_timeout: float = 60.0) -> dict:
        """Wait for QGC/PX4 readiness, then retry transiently rejected arm requests."""
        deadline = time.monotonic() + max(0.0, retry_timeout)
        attempts = []
        expected_count = max(1, self._expected_count)

        def all_ready(current: dict) -> bool:
            vehicles = current.get("vehicles", [])
            return len(vehicles) >= expected_count and all(
                vehicle.get("initial_connect_complete", False)
                and (
                    not vehicle.get("ready_to_fly_available", False)
                    or vehicle.get("ready_to_fly", False)
                )
                for vehicle in vehicles
            )

        while True:
            remaining = max(0.0, deadline - time.monotonic())
            readiness = self.client.wait_status(all_ready, remaining)
            if not all_ready(readiness):
                return {
                    "pass": False,
                    "response": {},
                    "vehicles": readiness.get("vehicles", []),
                    "attempts": attempts,
                    "readiness_timeout": True,
                }

            response = self.client.request("arm_all")
            remaining = max(0.0, deadline - time.monotonic())
            status = self.client.wait_status(
                lambda current: len(current.get("vehicles", [])) >= expected_count
                and all(vehicle.get("armed") for vehicle in current.get("vehicles", [])),
                min(5.0, remaining),
            )
            vehicles = status.get("vehicles", [])
            passed = (
                bool(response.get("ok"))
                and len(vehicles) >= expected_count
                and all(vehicle.get("armed") for vehicle in vehicles)
            )
            attempts.append(
                {
                    "elapsed_sec": round(retry_timeout - max(0.0, deadline - time.monotonic()), 1),
                    "response": response,
                    "vehicles": vehicles,
                }
            )
            if passed or not response.get("ok") or time.monotonic() >= deadline:
                return {
                    "pass": passed,
                    "response": response,
                    "vehicles": vehicles,
                    "attempts": attempts,
                }

    def takeoff_all(self, altitude: float) -> dict:
        response = self.client.request("takeoff_all", altitude_m=altitude)
        return {"pass": bool(response.get("ok")), "response": response}

    def rtl_all(self) -> dict:
        response = self.client.request("rtl_all")
        return {"pass": bool(response.get("ok")), "response": response}


def _pid_of_instance(instance: int) -> int:
    """按启动命令行特征查找 SITL 进程 PID（px4 -i N）。"""
    out = subprocess.run(["pgrep", "-f", f"px4 -i {instance}"], capture_output=True, text=True)
    pids = [int(p) for p in out.stdout.split() if p.strip()]
    return pids[0] if pids else -1


def _process_environment_value(pid: int, name: str) -> str:
    try:
        entries = Path(f"/proc/{pid}/environ").read_bytes().split(b"\0")
    except OSError:
        return ""
    prefix = f"{name}=".encode()
    for entry in entries:
        if entry.startswith(prefix):
            return entry[len(prefix) :].decode(errors="replace")
    return ""


def _remove_gazebo_model(instance: int, px4_pid: int) -> dict:
    """安全移除失效机模型，避免无控制机体使共享 ODE 世界崩溃。"""
    model = f"x500_{instance}"
    environment = os.environ.copy()
    partition = _process_environment_value(px4_pid, "GZ_PARTITION")
    if partition:
        environment["GZ_PARTITION"] = partition
    proc = subprocess.run(
        [
            "gz",
            "service",
            "-s",
            "/world/default/remove",
            "--reqtype",
            "gz.msgs.Entity",
            "--reptype",
            "gz.msgs.Boolean",
            "--timeout",
            "5000",
            "--req",
            f'name: "{model}" type: MODEL',
        ],
        capture_output=True,
        text=True,
        env=environment,
    )
    response = (proc.stdout or proc.stderr).strip()
    return {
        "model": model,
        "returncode": proc.returncode,
        "response": response,
        "partition": partition,
        "pass": proc.returncode == 0 and "data: true" in response,
    }


def run_scenario(
    scenario: dict,
    conn_str: str,
    dry_run: bool,
    qgc_client: QGCAcceptanceClient | None = None,
) -> dict:
    fleet_size = scenario.get("fleet", 8)
    expected_online = fleet_size
    report = {"scenario": scenario["name"], "steps": [], "pass": True}
    t_start = time.time()
    requires_qgc = bool(scenario.get("requires_qgc", False))
    if dry_run:
        fleet = None
    elif requires_qgc and qgc_client:
        fleet = QGCFleet(qgc_client)
    elif requires_qgc:
        return {
            "scenario": scenario["name"],
            "steps": [],
            "pass": False,
            "error": "scenario requires the QGC acceptance interface",
            "total_sec": 0.0,
        }
    else:
        fleet = Fleet(conn_str)

    def record(action_desc: str, ok: bool, extra: dict | None = None):
        entry = {"action": action_desc, "ok": ok, "elapsed_sec": round(time.time() - t_start, 1)}
        if extra:
            entry.update(extra)
        report["steps"].append(entry)
        report["pass"] = report["pass"] and ok
        print(f"  [{'OK' if ok else 'FAIL'}] {action_desc}（{entry['elapsed_sec']}s）")

    for step in scenario.get("steps", []):
        wait_t = step.get("t", 0)
        if wait_t and not dry_run:
            fleet.pump(wait_t)
        action = step["action"]
        desc = action + (
            " "
            + json.dumps(
                {k: v for k, v in step.items() if k not in ("t", "action")}, ensure_ascii=False
            )
            if len(step) > 2
            else ""
        )
        if dry_run:
            record(desc, True, {"dry_run": True})
            continue

        if action == "wait_all_online":
            record(desc, fleet.wait_all_online(fleet_size, step.get("timeout_sec", 120)))
        elif action == "arm_all":
            result = fleet.arm_all(float(step.get("retry_timeout_sec", 60)))
            record(desc, result["pass"], result)
        elif action == "takeoff_all":
            if isinstance(fleet, QGCFleet):
                result = fleet.takeoff_all(float(step.get("altitude", 30)))
            else:
                m = fleet.mavutil.mavlink
                result = fleet.broadcast(
                    m.MAV_CMD_NAV_TAKEOFF,
                    p1=float("nan"),
                    p2=float("nan"),
                    p3=0.0,
                    p4=float("nan"),
                    p5=float("nan"),
                    p6=float("nan"),
                    p7=float(step.get("altitude", 30)),
                    relative_altitude=True,
                )
            record(desc, result["pass"], result)
        elif action == "rtl_all":
            if isinstance(fleet, QGCFleet):
                result = fleet.rtl_all()
            else:
                result = fleet.broadcast(fleet.mavutil.mavlink.MAV_CMD_NAV_RETURN_TO_LAUNCH)
            record(desc, result["pass"], result)
        elif action == "wait_all_airborne":
            ok = fleet.wait_all_airborne(
                step.get("min_altitude", 3), step.get("timeout_sec", 90), expected_online
            )
            record(desc, ok, fleet.last_condition_detail)
        elif action == "wait_all_disarmed_or_low":
            ok = fleet.wait_all_disarmed_or_low(
                step.get("max_altitude", 2), step.get("timeout_sec", 120), expected_online
            )
            record(desc, ok, fleet.last_condition_detail)
        elif action == "wait_seconds":
            fleet.pump(float(step.get("seconds", 1)))
            record(desc, True)
        elif action == "kill_instance":
            pid = _pid_of_instance(int(step["instance"]))
            removal = (
                _remove_gazebo_model(int(step["instance"]), pid)
                if pid > 0
                else {
                    "pass": False,
                    "error": "PX4 instance process not found",
                }
            )
            if pid > 0 and removal["pass"]:
                fleet.pump(1.0)
                os.kill(pid, signal.SIGKILL)
            record(desc, pid > 0 and removal["pass"], {"pid": pid, "gazebo_model_removal": removal})
        elif action == "freeze_instance":
            pid = _pid_of_instance(int(step["instance"]))
            ok = pid > 0
            if ok:
                os.kill(pid, signal.SIGSTOP)
                fleet.pump(float(step.get("seconds", 10)))
                os.kill(pid, signal.SIGCONT)
            record(desc, ok, {"pid": pid, "seconds": step.get("seconds", 10)})
        elif action == "degrade_mavlink":
            result = fleet.degrade_mavlink(
                int(step["instance"]),
                float(step.get("seconds", 10)),
                expected_online,
                float(step.get("recovery_timeout_sec", 15)),
            )
            record(desc, result["pass"], result)
        elif action == "check_online_count":
            expect = int(step["expect"])
            actual = len(fleet.online_ids())
            record(desc, actual == expect, {"expect": expect, "actual": actual})
            if actual == expect:
                expected_online = expect
        elif action == "measure_pause_latency":
            result = fleet.measure_pause_latency(int(step.get("threshold_ms", 1000)))
            report["pause_latency"] = result
            record(desc, result["pass"], result)
        elif action == "qgc_plan_coverage":
            if not qgc_client:
                record(desc, False, {"error": "QGC acceptance interface is required"})
                continue
            fleet.pump(1.0)
            coordinates = [
                fleet.position[sid] for sid in fleet.online_ids() if sid in fleet.position
            ]
            if not coordinates:
                record(desc, False, {"error": "fleet positions are unavailable"})
                continue
            center_lat = sum(point[0] for point in coordinates) / len(coordinates)
            center_lon = sum(point[1] for point in coordinates) / len(coordinates)
            try:
                response = qgc_client.request(
                    "plan_coverage",
                    center_lat=center_lat,
                    center_lon=center_lon,
                    length_m=float(step.get("length_m", 120.0)),
                    width_m=float(step.get("width_m", 80.0)),
                    angle_deg=float(step.get("angle_deg", 0.0)),
                    assign_mode=step.get("assign_mode", "sequential"),
                )
                mission_vehicle_count = expected_online
                status = qgc_client.wait_status(
                    lambda current, required_count=mission_vehicle_count: len(
                        current.get("mission", {}).get("status", {})
                    )
                    >= required_count,
                    float(step.get("timeout_sec", 60.0)),
                )
                mission = status.get("mission", {})
                uploaded = mission.get("status", {})
                ok = (
                    response.get("ok")
                    and mission.get("vehicle_count") == expected_online
                    and len(uploaded) >= expected_online
                    and all(int(count) >= 3 for count in uploaded.values())
                )
                record(desc, ok, {"response": response, "mission": mission})
            except QGCAcceptanceError as exc:
                record(desc, False, {"error": str(exc)})
        elif action == "qgc_start_mission":
            if not qgc_client:
                record(desc, False, {"error": "QGC acceptance interface is required"})
                continue
            try:
                response = qgc_client.request("start_mission")
                record(desc, bool(response.get("ok")), {"response": response})
            except QGCAcceptanceError as exc:
                record(desc, False, {"error": str(exc)})
        elif action == "qgc_start_formation":
            if not qgc_client:
                record(desc, False, {"error": "QGC acceptance interface is required"})
                continue
            try:
                response = qgc_client.request(
                    "start_formation",
                    leader_id=int(step.get("leader_id", 1)),
                    type=step.get("type", "wedge"),
                    spacing_m=float(step.get("spacing_m", 10.0)),
                    vertical_step_m=float(step.get("vertical_step_m", 3.0)),
                )
                minimum_followers = int(step.get("min_followers", expected_online - 1))
                status = qgc_client.wait_status(
                    lambda current, required_count=minimum_followers: current.get(
                        "formation", {}
                    ).get("active")
                    and current.get("formation", {}).get("follower_count", 0) >= required_count,
                    float(step.get("timeout_sec", 15.0)),
                )
                ok = bool(response.get("ok")) and bool(status.get("formation", {}).get("active"))
                record(desc, ok, {"response": response, "formation": status.get("formation", {})})
            except QGCAcceptanceError as exc:
                record(desc, False, {"error": str(exc)})
        elif action == "qgc_check_formation":
            if not qgc_client:
                record(desc, False, {"error": "QGC acceptance interface is required"})
                continue
            try:
                status = qgc_client.request("status")
                metrics = formation_geometry_metrics(status)
                max_horizontal = metrics.get("max_horizontal_error_m")
                max_vertical = metrics.get("max_vertical_error_m")
                ok = (
                    metrics.get("pass")
                    and metrics.get("follower_count", 0) >= int(step.get("min_followers", 1))
                    and max_horizontal is not None
                    and max_horizontal <= float(step.get("max_horizontal_error_m", 8.0))
                    and max_vertical is not None
                    and max_vertical <= float(step.get("max_vertical_error_m", 4.0))
                )
                record(desc, ok, {"formation_metrics": metrics})
            except QGCAcceptanceError as exc:
                record(desc, False, {"error": str(exc)})
        elif action == "qgc_stop_formation":
            if not qgc_client:
                record(desc, False, {"error": "QGC acceptance interface is required"})
                continue
            try:
                response = qgc_client.request("stop_formation")
                record(desc, bool(response.get("ok")), {"response": response})
            except QGCAcceptanceError as exc:
                record(desc, False, {"error": str(exc)})
        elif action == "qgc_execute_actions":
            if not qgc_client:
                record(desc, False, {"error": "QGC acceptance interface is required"})
                continue
            try:
                response = qgc_client.request(
                    "execute_actions",
                    origin=step.get("origin", "operator"),
                    actions=step.get("actions", []),
                    center_lat=float(step.get("center_lat", 0.0)),
                    center_lon=float(step.get("center_lon", 0.0)),
                )
                record(desc, bool(response.get("ok")), {"response": response})
            except QGCAcceptanceError as exc:
                record(desc, False, {"error": str(exc)})
        elif action == "qgc_set_flight_mode":
            if not qgc_client:
                record(desc, False, {"error": "QGC acceptance interface is required"})
                continue
            try:
                response = qgc_client.request(
                    "set_flight_mode",
                    vehicle_id=int(step["vehicle_id"]),
                    mode=step.get("mode", ""),
                    mode_kind=step.get("mode_kind", ""),
                )
                expected_mode = response.get("requested_mode")
                target_vehicle_id = int(step["vehicle_id"])
                status = qgc_client.wait_status(
                    lambda current, target_id=target_vehicle_id, target_mode=expected_mode: any(
                        vehicle.get("id") == target_id and vehicle.get("flight_mode") == target_mode
                        for vehicle in current.get("vehicles", [])
                    ),
                    float(step.get("timeout_sec", 10.0)),
                )
                changed = any(
                    vehicle.get("id") == int(step["vehicle_id"])
                    and vehicle.get("flight_mode") == expected_mode
                    for vehicle in status.get("vehicles", [])
                )
                record(desc, bool(response.get("ok")) and changed, {"response": response})
            except QGCAcceptanceError as exc:
                record(desc, False, {"error": str(exc)})
        elif action == "qgc_expect_lease":
            if not qgc_client:
                record(desc, False, {"error": "QGC acceptance interface is required"})
                continue
            vehicle_id = int(step["vehicle_id"])
            expected_state = step["state"]
            try:
                status = qgc_client.wait_status(
                    lambda current, target_id=vehicle_id, target_state=expected_state: any(
                        vehicle.get("id") == target_id
                        and vehicle.get("lease_state") == target_state
                        for vehicle in current.get("vehicles", [])
                    ),
                    float(step.get("timeout_sec", 10.0)),
                )
                vehicle_status = next(
                    (
                        vehicle
                        for vehicle in status.get("vehicles", [])
                        if vehicle.get("id") == vehicle_id
                    ),
                    {},
                )
                record(
                    desc,
                    vehicle_status.get("lease_state") == expected_state,
                    {"vehicle_status": vehicle_status},
                )
            except QGCAcceptanceError as exc:
                record(desc, False, {"error": str(exc)})
        elif action == "qgc_start_task":
            if not qgc_client:
                record(desc, False, {"error": "QGC acceptance interface is required"})
                continue
            try:
                response = qgc_client.request(
                    "start_task",
                    name=str(step.get("name", "acceptance_task")),
                    center_lat=float(step.get("center_lat", 39.9)),
                    center_lon=float(step.get("center_lon", 116.4)),
                    stages=step.get("stages", []),
                )
                record(desc, bool(response.get("ok")), {"response": response})
            except QGCAcceptanceError as exc:
                record(desc, False, {"error": str(exc)})
        elif action == "qgc_expect_task":
            if not qgc_client:
                record(desc, False, {"error": "QGC acceptance interface is required"})
                continue
            expected = {
                key: step[key]
                for key in ("running", "name", "stage_index", "waiting_for_proceed")
                if key in step
            }
            try:
                status = qgc_client.wait_status(
                    lambda current, wanted=expected: all(
                        current.get("task", {}).get(key) == value for key, value in wanted.items()
                    ),
                    float(step.get("timeout_sec", 10.0)),
                )
                task = status.get("task", {})
                ok = bool(status.get("ok")) and all(
                    task.get(key) == value for key, value in expected.items()
                )
                record(desc, ok, {"task": task, "expected": expected})
            except QGCAcceptanceError as exc:
                record(desc, False, {"error": str(exc)})
        elif action == "qgc_proceed_task":
            if not qgc_client:
                record(desc, False, {"error": "QGC acceptance interface is required"})
                continue
            try:
                response = qgc_client.request("proceed_task")
                record(desc, bool(response.get("ok")), {"response": response})
            except QGCAcceptanceError as exc:
                record(desc, False, {"error": str(exc)})
        elif action == "qgc_abort_task":
            if not qgc_client:
                record(desc, False, {"error": "QGC acceptance interface is required"})
                continue
            try:
                response = qgc_client.request("abort_task")
                record(desc, bool(response.get("ok")), {"response": response})
            except QGCAcceptanceError as exc:
                record(desc, False, {"error": str(exc)})
        elif action == "note":
            record(f"note: {step.get('text', '')}", True)
        elif action == "perception_feed":
            # 感知链路馈送（WP3.2/3.7）：以服务进程实时模式跑场景脚本，
            # UDP 输出至 15600（QGC SwarmPerceptionBridge 默认端口）
            import subprocess

            svc = os.path.join(
                os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                "services",
                "perception",
                "service.py",
            )
            scn = os.path.join(
                os.path.dirname(os.path.abspath(__file__)),
                "scenarios",
                os.path.basename(step.get("scenario", "")),
            )
            proc = subprocess.Popen(
                [
                    sys.executable,
                    svc,
                    "--scenario",
                    scn,
                    "--out",
                    "udp://127.0.0.1:15600",
                    "--realtime",
                    "--quiet",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            observation_started = time.monotonic()
            observation_deadline = (
                observation_started + float(scenario.get("duration_s", 30)) + 30.0
            )
            first_model_ms = None
            first_delegate_ms = None
            render_samples = []
            best_truth_errors = {}
            last_observed_status = {}
            while proc.poll() is None and time.monotonic() < observation_deadline:
                if qgc_client:
                    try:
                        last_observed_status = qgc_client.request("status")
                        observed_cop = last_observed_status.get("cop", {})
                        elapsed_ms = round((time.monotonic() - observation_started) * 1000.0, 1)
                        model_count = int(observed_cop.get("track_count", 0))
                        delegate_count = int(observed_cop.get("rendered_track_count", 0))
                        render_samples.append(
                            {
                                "elapsed_ms": elapsed_ms,
                                "model_count": model_count,
                                "delegate_count": delegate_count,
                            }
                        )
                        if model_count > 0 and first_model_ms is None:
                            first_model_ms = elapsed_ms
                        if delegate_count > 0 and first_delegate_ms is None:
                            first_delegate_ms = elapsed_ms
                        sample_truth_metrics = cop_truth_metrics(
                            scenario,
                            observed_cop.get("tracks", []),
                            elapsed_ms / 1000.0,
                        )
                        for truth_index, error_m in sample_truth_metrics.get(
                            "errors_by_truth_m", {}
                        ).items():
                            best_truth_errors[truth_index] = min(
                                float(error_m),
                                best_truth_errors.get(truth_index, math.inf),
                            )
                    except QGCAcceptanceError:
                        pass
                time.sleep(0.1)
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=5)
            service_output, _ = proc.communicate(timeout=5)
            service_output = service_output or ""
            match = re.search(
                r"完成：(\d+) 帧，(\d+) 次检测，(\d+) 条航迹报文，"
                r"(\d+) 次心跳，(\d+) 字节",
                service_output,
            )
            stats = {}
            if match:
                frames, detections, track_batches, heartbeats, wire_bytes = (
                    int(value) for value in match.groups()
                )
                duration = float(scenario.get("duration_s", 30))
                stats = {
                    "frames": frames,
                    "detections": detections,
                    "track_batches": track_batches,
                    "heartbeats": heartbeats,
                    "heartbeat_hz": round(heartbeats / duration, 3),
                    "wire_bytes": wire_bytes,
                }
            expect = step.get("expect", {})
            checked = {
                "min_track_batches": (
                    stats.get("track_batches", -1) >= int(expect.get("min_track_batches", 0))
                ),
                "min_heartbeat_hz": (
                    stats.get("heartbeat_hz", -1.0) >= float(expect.get("min_heartbeat_hz", 0))
                ),
            }
            qgc_status = last_observed_status
            if qgc_client:
                try:
                    qgc_status = qgc_client.wait_status(
                        lambda current: current.get("cop", {}).get("track_count", 0) > 0,
                        10.0,
                    )
                except QGCAcceptanceError:
                    qgc_status = {}
            perception = qgc_status.get("perception", {})
            cop = qgc_status.get("cop", {})
            ui = qgc_status.get("ui", {})
            truth_count = sum(bool(truth.get("waypoints")) for truth in scenario.get("truths", []))
            truth_errors = [
                round(best_truth_errors[str(index)], 2)
                for index in range(truth_count)
                if str(index) in best_truth_errors
            ]
            truth_metrics = {
                "pass": len(truth_errors) == truth_count and truth_count > 0,
                "matched_truths": len(truth_errors),
                "truth_count": truth_count,
                "errors_m": truth_errors,
                "errors_by_truth_m": {
                    key: round(value, 2) for key, value in best_truth_errors.items()
                },
                "max_error_m": max(truth_errors, default=None),
                "measurement": "best error per truth over observation window",
            }
            checked.update(
                {
                    "max_geo_error_m": (
                        truth_metrics.get("pass", False)
                        and truth_metrics.get("max_error_m") is not None
                        and truth_metrics["max_error_m"]
                        <= float(expect.get("max_geo_error_m", math.inf))
                    ),
                    "max_e2e_latency_ms": (
                        perception.get("wire_latency_ms") is not None
                        and float(perception.get("wire_latency_ms", math.inf))
                        <= float(expect.get("max_e2e_latency_ms", math.inf))
                    ),
                    "qgc_cop_model": cop.get("track_count", 0) > 0,
                    "qgc_cop_rendering": (
                        cop.get("rendered_track_count", 0) == cop.get("track_count", 0)
                    ),
                    "qgc_situation_layer": ui.get("situation_layer_count", 0) == 1,
                }
            )
            unverified = []
            ok = proc.returncode == 0 and bool(match) and all(checked.values())
            record(
                desc,
                ok,
                {
                    "service_tail": service_output[-240:],
                    "service_stats": stats,
                    "checked_expectations": checked,
                    "unverified_expectations": unverified,
                    "qgc_perception": perception,
                    "qgc_cop": cop,
                    "qgc_ui": ui,
                    "render_observation": {
                        "first_model_ms": first_model_ms,
                        "first_delegate_ms": first_delegate_ms,
                        "sample_count": len(render_samples),
                        "samples": render_samples,
                    },
                    "truth_metrics": truth_metrics,
                },
            )
        else:
            record(f"{action}（未知动作，跳过）", False)

    report["total_sec"] = round(time.time() - t_start, 1)
    criteria = scenario.get("pass_criteria", {})
    if "max_total_sec" in criteria and report["total_sec"] > criteria["max_total_sec"]:
        report["pass"] = False
    return report


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("scenario", help="场景 JSON 路径")
    ap.add_argument("--conn", default="udpin:0.0.0.0:14550", help="MAVLink 连接串")
    ap.add_argument("--report", default="", help="报告输出路径")
    ap.add_argument("--dry-run", action="store_true", help="只解析打印时间线，不连接机队")
    ap.add_argument("--qgc-host", default="", help="QGC 验收接口地址；留空则不连接 QGC L2")
    ap.add_argument("--qgc-port", type=int, default=15610, help="QGC 验收接口端口")
    ap.add_argument(
        "--qgc-token-file",
        default=os.environ.get("QGC_SWARM_ACCEPTANCE_TOKEN_FILE", ""),
        help="QGC 验收接口 Secret 文件",
    )
    args = ap.parse_args()

    scenario = json.loads(Path(args.scenario).read_text(encoding="utf-8"))
    print(
        f"[run_scenario] {scenario['name']}（机队 {scenario.get('fleet', 8)} 架）"
        f"{'（dry-run）' if args.dry_run else ''}"
    )

    qgc_client = None
    if args.qgc_host:
        if not args.qgc_token_file:
            ap.error("连接 QGC 验收接口需要 --qgc-token-file")
        try:
            token = read_acceptance_token(args.qgc_token_file)
        except ValueError as exc:
            ap.error(str(exc))
        qgc_client = QGCAcceptanceClient(args.qgc_host, args.qgc_port, token)
    report = run_scenario(scenario, args.conn, args.dry_run, qgc_client)
    text = json.dumps(report, ensure_ascii=False, indent=2)
    if args.report:
        Path(args.report).write_text(text, encoding="utf-8")
    print(
        f"[run_scenario] 结果：{'通过' if report['pass'] else '未通过'}"
        f"（总耗时 {report['total_sec']}s）"
    )
    sys.exit(0 if report["pass"] else 1)


if __name__ == "__main__":
    main()
