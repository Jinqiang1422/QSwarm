#!/usr/bin/env python3
# -*- coding: utf-8 -*-
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
import argparse
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

HEARTBEAT_TIMEOUT_SEC = 5.0   # 超过该时长未收到心跳视为离线


class Fleet:
    """MAVLink 机群连接：单 UDP 入口按 sysid 区分各机（与 QGC 接入机制一致）。"""

    def __init__(self, conn_str: str):
        from pymavlink import mavutil  # 延迟导入，dry-run 无需依赖
        self.mav = mavutil.mavlink_connection(conn_str, autoreconnect=True)
        self.mavutil = mavutil
        self.last_heartbeat = {}      # sysid -> (timestamp, custom_mode, base_mode)
        self.rel_alt = {}             # sysid -> 相对高度（米）

    def pump(self, duration: float = 0.0) -> None:
        """处理积压消息并（可选）等待 duration 秒。"""
        deadline = time.time() + duration
        while True:
            msg = self.mav.recv_match(blocking=True, timeout=0.2)
            if msg is None:
                if time.time() >= deadline:
                    return
                continue
            sid = msg.get_srcSystem()
            mtype = msg.get_type()
            if mtype == "HEARTBEAT":
                self.last_heartbeat[sid] = (time.time(), msg.custom_mode, msg.base_mode)
            elif mtype == "GLOBAL_POSITION_INT":
                self.rel_alt[sid] = msg.relative_alt / 1000.0
            if time.time() >= deadline:
                return

    def online_ids(self) -> list:
        now = time.time()
        return sorted(sid for sid, (t, _, _) in self.last_heartbeat.items()
                      if now - t < HEARTBEAT_TIMEOUT_SEC)

    def wait_all_online(self, fleet: int, timeout: float) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.pump(1.0)
            if len(self.online_ids()) >= fleet:
                return True
        return False

    def wait_all_airborne(self, min_alt: float, timeout: float) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.pump(1.0)
            online = self.online_ids()
            if online and all(self.rel_alt.get(sid, 0.0) >= min_alt for sid in online):
                return True
        return False

    def wait_all_disarmed_or_low(self, max_alt: float, timeout: float) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.pump(1.0)
            online = self.online_ids()
            if online and all(self.rel_alt.get(sid, 999.0) <= max_alt for sid in online):
                return True
        return False

    def send_command(self, sysid: int, command: int, p1=0.0, p2=0.0, p7=0.0) -> None:
        m = self.mavutil.mavlink
        self.mav.mav.command_long_send(
            sysid, 1, command, 0, p1, p2, 0, 0, 0, 0, p7)

    def broadcast(self, command: int, p1=0.0, p2=0.0, p7=0.0) -> None:
        for sid in self.online_ids():
            self.send_command(sid, command, p1, p2, p7)

    def measure_pause_latency(self, threshold_ms: int) -> dict:
        """对全体下发暂停（DO_REPOSITION 悬停），测各机 COMMAND_ACK 时延。"""
        m = self.mavutil.mavlink
        targets = self.online_ids()
        t0 = time.time()
        pending = set(targets)
        for sid in targets:
            self.mav.mav.command_long_send(
                sid, 1, m.MAV_CMD_DO_REPOSITION, 0,
                -1, 0, 0, 0, float("nan"), float("nan"), float("nan"))
        latencies = {}
        deadline = t0 + threshold_ms / 1000.0 * 3
        while pending and time.time() < deadline:
            msg = self.mav.recv_match(type="COMMAND_ACK", blocking=True, timeout=0.2)
            if msg is None:
                continue
            sid = msg.get_srcSystem()
            if sid in pending and msg.command == m.MAV_CMD_DO_REPOSITION:
                pending.discard(sid)
                latencies[sid] = round((time.time() - t0) * 1000, 1)
        for sid in pending:
            latencies[sid] = None   # 超时未确认
        values = [v for v in latencies.values() if v is not None]
        return {
            "per_vehicle_ms": latencies,
            "max_ms": max(values) if values else None,
            "pass": bool(values) and max(values) <= threshold_ms and not pending,
            "threshold_ms": threshold_ms,
        }


def _pid_of_instance(instance: int) -> int:
    """按启动命令行特征查找 SITL 进程 PID（px4 -i N）。"""
    out = subprocess.run(["pgrep", "-f", f"px4 -i {instance}"],
                         capture_output=True, text=True)
    pids = [int(p) for p in out.stdout.split() if p.strip()]
    return pids[0] if pids else -1


def run_scenario(scenario: dict, conn_str: str, dry_run: bool) -> dict:
    fleet_size = scenario.get("fleet", 8)
    report = {"scenario": scenario["name"], "steps": [], "pass": True}
    t_start = time.time()
    fleet = None if dry_run else Fleet(conn_str)

    def record(action_desc: str, ok: bool, extra: dict = None):
        entry = {"action": action_desc, "ok": ok,
                 "elapsed_sec": round(time.time() - t_start, 1)}
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
        desc = action + (" " + json.dumps({k: v for k, v in step.items()
                                           if k not in ("t", "action")}, ensure_ascii=False)
                         if len(step) > 2 else "")
        if dry_run:
            record(desc, True, {"dry_run": True})
            continue

        m = fleet.mavutil.mavlink
        if action == "wait_all_online":
            record(desc, fleet.wait_all_online(fleet_size, step.get("timeout_sec", 120)))
        elif action == "arm_all":
            fleet.broadcast(m.MAV_CMD_COMPONENT_ARM_DISARM, p1=1.0, p2=0.0)
            record(desc, True)
        elif action == "takeoff_all":
            fleet.broadcast(m.MAV_CMD_NAV_TAKEOFF, p7=float(step.get("altitude", 30)))
            record(desc, True)
        elif action == "rtl_all":
            fleet.broadcast(m.MAV_CMD_NAV_RETURN_TO_LAUNCH)
            record(desc, True)
        elif action == "wait_all_airborne":
            record(desc, fleet.wait_all_airborne(step.get("min_altitude", 3),
                                                 step.get("timeout_sec", 90)))
        elif action == "wait_all_disarmed_or_low":
            record(desc, fleet.wait_all_disarmed_or_low(step.get("max_altitude", 2),
                                                        step.get("timeout_sec", 120)))
        elif action == "wait_seconds":
            fleet.pump(float(step.get("seconds", 1)))
            record(desc, True)
        elif action == "kill_instance":
            pid = _pid_of_instance(int(step["instance"]))
            if pid > 0:
                os.kill(pid, signal.SIGKILL)
            record(desc, pid > 0, {"pid": pid})
        elif action == "freeze_instance":
            pid = _pid_of_instance(int(step["instance"]))
            ok = pid > 0
            if ok:
                os.kill(pid, signal.SIGSTOP)
                fleet.pump(float(step.get("seconds", 10)))
                os.kill(pid, signal.SIGCONT)
            record(desc, ok, {"pid": pid, "seconds": step.get("seconds", 10)})
        elif action == "check_online_count":
            expect = int(step["expect"])
            actual = len(fleet.online_ids())
            record(desc, actual == expect, {"expect": expect, "actual": actual})
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
            svc = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                               "services", "perception", "service.py")
            scn = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenarios",
                               os.path.basename(step.get("scenario", "")))
            proc = subprocess.run(
                [sys.executable, svc, "--scenario", scn,
                 "--out", "udp://127.0.0.1:15600", "--realtime", "--quiet"],
                capture_output=True, text=True,
                timeout=float(scenario.get("duration_s", 30)) + 30.0)
            ok = proc.returncode == 0
            record(desc, ok, {"service_tail": (proc.stdout or proc.stderr)[-200:]})
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
    print(f"[run_scenario] {scenario['name']}（机队 {scenario.get('fleet', 8)} 架）"
          f"{'（dry-run）' if args.dry_run else ''}")

    report = run_scenario(scenario, args.conn, args.dry_run)
    text = json.dumps(report, ensure_ascii=False, indent=2)
    if args.report:
        Path(args.report).write_text(text, encoding="utf-8")
    print(f"[run_scenario] 结果：{'通过' if report['pass'] else '未通过'}"
          f"（总耗时 {report['total_sec']}s）")
    sys.exit(0 if report["pass"] else 1)


if __name__ == "__main__":
    main()
