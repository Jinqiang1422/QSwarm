#!/usr/bin/env python3
# ruff: noqa: RUF001, RUF002, RUF003
"""regression.py — 一键跑完 scenarios/ 所有场景并汇总 pass/fail + 关键指标。

复用 run_scenario.py 的驱动逻辑，遍历全部场景 → 逐个跑 → 收集报告 →
生成 Markdown 汇总表(结果 / 耗时 / 一键中止时延等) + 退出码(全过 0 / 有失败 1)。

前置：SITL 机队已就绪(如 ./run_official_sitl.sh ~/PX4-Autopilot 8)，
      场景 fleet 与实际机数一致；mavlink 聚合在宿主 14550。严格飞行验收应由外层为
      每个场景重启全新机队；本脚本连续在线执行只适合明确接受跨场景状态复用的场合。

用法：
  python3 regression.py                              # 全跑
  python3 regression.py --only 01,05                 # 只跑 01/05
  python3 regression.py --dry-run                    # 不连机队，仅校验场景定义+自检脚本
  python3 regression.py --conn udpin:0.0.0.0:14550 --out /tmp/sitl_regression
"""

import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_scenario as rs  # 复用同目录驱动


def _metric_of(report: dict) -> str:
    """从单场景报告提取一句话关键指标(有则显示)。"""
    bits = []
    if "pause_latency" in report:
        pl = report["pause_latency"]
        ok = "≤" if pl.get("pass") else ">"
        bits.append(f"一键中止 max={pl.get('max_ms')}ms {ok}{pl.get('threshold_ms')}ms")
    # 在线计数类步骤(故障隔离场景)
    for step in report.get("steps", []):
        if step.get("action", "").startswith("check_online_count") and "actual" in step:
            bits.append(f"在线 {step['actual']}/{step['expect']}")
        if step.get("action", "").startswith("degrade_mavlink") and "online_during_drop" in step:
            during = len(step.get("online_during_drop", []))
            after = len(step.get("online_after_recovery", []))
            bits.append(f"链路在线 {during}→{after}")
        if step.get("action", "").startswith("perception_feed"):
            stats = step.get("service_stats", {})
            if stats:
                bits.append(
                    f"航迹批次 {stats.get('track_batches')}；心跳 {stats.get('heartbeat_hz')}Hz"
                )
    return "；".join(bits)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--conn", default="udpin:0.0.0.0:14550", help="MAVLink 连接串")
    ap.add_argument("--out", default="/tmp/sitl_regression", help="报告输出目录")
    ap.add_argument("--only", default="", help="逗号分隔场景号(如 01,05)，默认全跑")
    ap.add_argument("--dry-run", action="store_true", help="不连机队，仅解析场景+自检")
    ap.add_argument("--qgc-host", default="", help="QGC 验收接口地址；留空则不连接 QGC L2")
    ap.add_argument("--qgc-port", type=int, default=15610, help="QGC 验收接口端口")
    ap.add_argument(
        "--qgc-token-file",
        default=os.environ.get("QGC_SWARM_ACCEPTANCE_TOKEN_FILE", ""),
        help="QGC 验收接口 Secret 文件",
    )
    args = ap.parse_args()

    sim_dir = Path(__file__).resolve().parent
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    scenarios = sorted((sim_dir / "scenarios").glob("*.json"))
    if args.only:
        keys = [k.strip() for k in args.only.split(",") if k.strip()]
        scenarios = [s for s in scenarios if any(s.name.startswith(k) for k in keys)]
    if not scenarios:
        print("未找到匹配的场景 JSON", file=sys.stderr)
        sys.exit(2)

    # kill_instance 会永久移除本轮中的一个 SITL 实例。将这类破坏性场景排到最后，
    # 避免后续场景因机队已不完整而产生与被测功能无关的级联失败。
    def is_destructive(path: Path) -> bool:
        try:
            scenario = json.loads(path.read_text(encoding="utf-8"))
        except Exception:  # 解析错误仍留给主循环生成正式失败报告
            return False
        return any(step.get("action") == "kill_instance" for step in scenario.get("steps", []))

    scenarios.sort(key=lambda path: (is_destructive(path), path.name))

    if not args.dry_run and len(scenarios) > 1:
        print(
            "[regression] 警告：多个在线飞行场景将复用同一机队；严格验收请由外层逐场景重启 "
            "PX4/Gazebo，并用 --only 每次执行一个场景。",
            file=sys.stderr,
        )

    rows = []
    qgc_client = None
    if args.qgc_host:
        if not args.qgc_token_file:
            ap.error("连接 QGC 验收接口需要 --qgc-token-file")
        try:
            token = rs.read_acceptance_token(args.qgc_token_file)
        except ValueError as exc:
            ap.error(str(exc))
        qgc_client = rs.QGCAcceptanceClient(args.qgc_host, args.qgc_port, token)
    for scn_path in scenarios:
        try:
            scn = json.loads(scn_path.read_text(encoding="utf-8"))
        except Exception as e:
            rows.append((scn_path.stem, "(解析失败)", "FAIL", 0, "", str(e)))
            continue
        print(
            f"\n===== 跑场景 {scn_path.stem}: {scn.get('name', '')} "
            f"(机队 {scn.get('fleet', 8)} 架){'（dry-run）' if args.dry_run else ''} =====",
            flush=True,
        )
        try:
            report = rs.run_scenario(scn, args.conn, args.dry_run, qgc_client)
        except Exception as e:  # 单场景异常不中断整轮回归
            report = {"pass": False, "error": str(e), "total_sec": 0}
        (out_dir / f"report_{scn_path.stem}.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        rows.append(
            (
                scn_path.stem,
                scn.get("name", ""),
                "PASS" if report.get("pass") else "FAIL",
                report.get("total_sec", 0),
                _metric_of(report),
                report.get("error", ""),
            )
        )

    n_pass = sum(1 for r in rows if r[2] == "PASS")
    n_fail = len(rows) - n_pass

    lines = [
        "# SITL 场景回归汇总" + ("（dry-run 自检）" if args.dry_run else ""),
        "",
        f"- 连接：`{args.conn}`",
        f"- 场景：{len(rows)}  ·  **PASS {n_pass} / FAIL {n_fail}**",
        "",
        "| 场景 | 名称 | 结果 | 耗时(s) | 关键指标 |",
        "|---|---|---|---|---|",
    ]
    for stem, name, res, sec, metric, err in rows:
        mark = "✅ PASS" if res == "PASS" else "❌ FAIL"
        note = f"{metric}" + (f" ⚠{err}" if err else "")
        lines.append(f"| {stem} | {name} | {mark} | {sec} | {note} |")
    summary = "\n".join(lines)

    (out_dir / "SUMMARY.md").write_text(summary + "\n", encoding="utf-8")
    print("\n" + summary)
    print(f"\n报告已写入：{out_dir}/SUMMARY.md（+ 各场景 report_*.json）")
    sys.exit(0 if n_fail == 0 else 1)


if __name__ == "__main__":
    main()
