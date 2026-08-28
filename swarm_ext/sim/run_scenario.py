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
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

HEARTBEAT_TIMEOUT_SEC = 5.0  # 超过该时长未收到心跳视为离线


class Fleet:
    """MAVLink 机群连接：单 UDP 入口按 sysid 区分各机（与 QGC 接入机制一致）。"""

    def __init__(self, conn_str: str):
        from pymavlink import mavutil  # 延迟导入，dry-run 无需依赖

        self.mav = mavutil.mavlink_connection(conn_str, autoreconnect=True)
        self.mavutil = mavutil
        self.last_heartbeat = {}  # sysid -> (timestamp, custom_mode, base_mode)
        self.rel_alt = {}  # sysid -> 相对高度（米）
        self.amsl_alt = {}  # sysid -> AMSL 高度（米）
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


def _pid_of_instance(instance: int) -> int:
    """按启动命令行特征查找 SITL 进程 PID（px4 -i N）。"""
    out = subprocess.run(["pgrep", "-f", f"px4 -i {instance}"], capture_output=True, text=True)
    pids = [int(p) for p in out.stdout.split() if p.strip()]
    return pids[0] if pids else -1


def _remove_gazebo_model(instance: int) -> dict:
    """安全移除失效机模型，避免无控制机体使共享 ODE 世界崩溃。"""
    model = f"x500_{instance}"
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
    )
    response = (proc.stdout or proc.stderr).strip()
    return {
        "model": model,
        "returncode": proc.returncode,
        "response": response,
        "pass": proc.returncode == 0 and "data: true" in response,
    }


def run_scenario(scenario: dict, conn_str: str, dry_run: bool) -> dict:
    fleet_size = scenario.get("fleet", 8)
    expected_online = fleet_size
    report = {"scenario": scenario["name"], "steps": [], "pass": True}
    t_start = time.time()
    fleet = None if dry_run else Fleet(conn_str)

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

        m = fleet.mavutil.mavlink
        if action == "wait_all_online":
            record(desc, fleet.wait_all_online(fleet_size, step.get("timeout_sec", 120)))
        elif action == "arm_all":
            result = fleet.arm_all(float(step.get("retry_timeout_sec", 60)))
            record(desc, result["pass"], result)
        elif action == "takeoff_all":
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
            result = fleet.broadcast(m.MAV_CMD_NAV_RETURN_TO_LAUNCH)
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
                _remove_gazebo_model(int(step["instance"]))
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
            proc = subprocess.run(
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
                capture_output=True,
                text=True,
                timeout=float(scenario.get("duration_s", 30)) + 30.0,
            )
            service_output = proc.stdout or proc.stderr
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
            unverified = sorted(
                key for key in expect if key not in ("min_track_batches", "min_heartbeat_hz")
            )
            ok = proc.returncode == 0 and bool(match) and all(checked.values())
            record(
                desc,
                ok,
                {
                    "service_tail": service_output[-240:],
                    "service_stats": stats,
                    "checked_expectations": checked,
                    "unverified_expectations": unverified,
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
    args = ap.parse_args()

    scenario = json.loads(Path(args.scenario).read_text(encoding="utf-8"))
    print(
        f"[run_scenario] {scenario['name']}（机队 {scenario.get('fleet', 8)} 架）"
        f"{'（dry-run）' if args.dry_run else ''}"
    )

    report = run_scenario(scenario, args.conn, args.dry_run)
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
