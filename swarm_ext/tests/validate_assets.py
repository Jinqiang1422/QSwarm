#!/usr/bin/env python3
# ruff: noqa: RUF001, RUF002, RUF003, UP038
"""Validate Swarm test assets and their single-source-of-truth contracts."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from dataclasses import asdict, dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SWARM_ROOT = REPO_ROOT / "swarm_ext"
COMPLETION_KINDS = {
    "all_airborne",
    "all_disarmed",
    "all_mission_finished",
    "duration",
    "formation_running",
    "manual",
}
PLACEHOLDER_RE = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")


@dataclass(frozen=True)
class Issue:
    severity: str
    asset: str
    message: str


def _load_json(path: Path, issues: list[Issue]) -> object | None:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        issues.append(Issue("error", str(path.relative_to(REPO_ROOT)), f"JSON 解析失败：{exc}"))
        return None


def _scenario_actions() -> set[str]:
    source = (SWARM_ROOT / "sim" / "run_scenario.py").read_text(encoding="utf-8")
    return set(re.findall(r'(?:if|elif) action == "([^"]+)"', source))


def _manager_actions() -> set[str]:
    source = (REPO_ROOT / "src" / "Swarm" / "SwarmManager.cc").read_text(encoding="utf-8")
    return set(re.findall(r'type == QStringLiteral\("([^"]+)"\)', source))


def validate_scenarios(issues: list[Issue]) -> dict[str, int]:
    allowed_actions = _scenario_actions()
    names: set[str] = set()
    step_count = 0
    scenario_paths = sorted((SWARM_ROOT / "sim" / "scenarios").glob("*.json"))
    for path in scenario_paths:
        relative = str(path.relative_to(REPO_ROOT))
        scenario = _load_json(path, issues)
        if not isinstance(scenario, dict):
            continue
        name = scenario.get("name")
        if not isinstance(name, str) or not name.strip():
            issues.append(Issue("error", relative, "缺少非空 name"))
        elif name in names:
            issues.append(Issue("error", relative, f"场景名称重复：{name}"))
        else:
            names.add(name)
        fleet = scenario.get("fleet", 8)
        if not isinstance(fleet, int) or not 0 <= fleet <= 32:
            issues.append(Issue("error", relative, f"fleet 必须是 0..32 的整数，实际为 {fleet!r}"))
        requires_qgc = bool(scenario.get("requires_qgc"))
        if scenario.get("requires_qml") and not requires_qgc:
            issues.append(Issue("error", relative, "requires_qml 场景必须同时声明 requires_qgc"))
        steps = scenario.get("steps", [])
        if not isinstance(steps, list):
            issues.append(Issue("error", relative, "steps 必须为数组"))
            continue
        step_count += len(steps)
        for index, step in enumerate(steps):
            if not isinstance(step, dict):
                issues.append(Issue("error", relative, f"steps[{index}] 必须为对象"))
                continue
            action = step.get("action")
            if action not in allowed_actions:
                issues.append(Issue("error", relative, f"steps[{index}] 使用未知动作：{action!r}"))
            if isinstance(action, str) and action.startswith("qgc_") and not requires_qgc:
                issues.append(
                    Issue("error", relative, f"steps[{index}] 使用 {action} 但未声明 requires_qgc")
                )
        criteria = scenario.get("pass_criteria", {})
        if criteria and not isinstance(criteria, dict):
            issues.append(Issue("error", relative, "pass_criteria 必须为对象"))
        elif (
            isinstance(criteria, dict)
            and "max_total_sec" in criteria
            and (
                not isinstance(criteria["max_total_sec"], (int, float))
                or criteria["max_total_sec"] <= 0
            )
        ):
            issues.append(Issue("error", relative, "max_total_sec 必须为正数"))
    return {"scenarios": len(scenario_paths), "scenario_steps": step_count}


def _embedded_templates(issues: list[Issue]) -> dict[str, dict]:
    source_path = REPO_ROOT / "src" / "Swarm" / "SwarmTaskLibrary.cc"
    source = source_path.read_text(encoding="utf-8")
    templates: dict[str, dict] = {}
    for index, raw_json in enumerate(re.findall(r'R"TPL\((.*?)\)TPL"', source, re.DOTALL)):
        try:
            item = json.loads(raw_json)
        except json.JSONDecodeError as exc:
            issues.append(
                Issue(
                    "error",
                    str(source_path.relative_to(REPO_ROOT)),
                    f"内嵌模板 #{index + 1} JSON 非法：{exc}",
                )
            )
            continue
        name = item.get("name")
        if isinstance(name, str):
            templates[name] = item
    return templates


def validate_templates(issues: list[Issue]) -> dict[str, int]:
    allowed_actions = _manager_actions()
    embedded = _embedded_templates(issues)
    names: set[str] = set()
    stage_count = 0
    template_paths = sorted((SWARM_ROOT / "templates").glob("*.json"))
    for path in template_paths:
        relative = str(path.relative_to(REPO_ROOT))
        template = _load_json(path, issues)
        if not isinstance(template, dict):
            continue
        name = template.get("name")
        if not isinstance(name, str) or not name.strip():
            issues.append(Issue("error", relative, "缺少非空 name"))
            continue
        if name in names:
            issues.append(Issue("error", relative, f"模板名称重复：{name}"))
        names.add(name)
        parameter_keys = {
            item.get("key")
            for item in template.get("parameters", [])
            if isinstance(item, dict) and isinstance(item.get("key"), str)
        }
        placeholders = set(PLACEHOLDER_RE.findall(json.dumps(template, ensure_ascii=False)))
        for missing_key in sorted(placeholders - parameter_keys):
            issues.append(Issue("error", relative, f"占位符 ${{{missing_key}}} 没有对应参数"))
        stages = template.get("stages")
        if not isinstance(stages, list) or not stages:
            issues.append(Issue("error", relative, "stages 必须为非空数组"))
            continue
        stage_count += len(stages)
        for index, stage in enumerate(stages):
            if not isinstance(stage, dict):
                issues.append(Issue("error", relative, f"stages[{index}] 必须为对象"))
                continue
            condition = stage.get("complete_when", {})
            kind = condition.get("kind") if isinstance(condition, dict) else None
            if kind not in COMPLETION_KINDS:
                issues.append(Issue("error", relative, f"stages[{index}] 完成条件无效：{kind!r}"))
            for action in stage.get("actions", []):
                action_type = action.get("type") if isinstance(action, dict) else None
                if action_type not in allowed_actions:
                    issues.append(
                        Issue("error", relative, f"stages[{index}] 使用未知动作：{action_type!r}")
                    )
        if embedded.get(name) != template:
            issues.append(Issue("error", relative, "磁盘模板与 SwarmTaskLibrary.cc 内嵌模板不一致"))
    for missing_name in sorted(set(embedded) - names):
        issues.append(
            Issue("error", "src/Swarm/SwarmTaskLibrary.cc", f"缺少磁盘模板：{missing_name}")
        )
    return {"templates": len(template_paths), "template_stages": stage_count}


def validate_source_inventory(issues: list[Issue]) -> dict[str, int]:
    source_dir = REPO_ROOT / "src" / "Swarm"
    cmake_text = (source_dir / "CMakeLists.txt").read_text(encoding="utf-8")
    source_paths = sorted(path for path in source_dir.iterdir() if path.suffix in {".cc", ".h"})
    for path in source_paths:
        if path.name not in cmake_text:
            issues.append(
                Issue("error", str(path.relative_to(REPO_ROOT)), "未列入 src/Swarm/CMakeLists.txt")
            )
    return {"swarm_sources": len(source_paths)}


def validate_generated_contracts(issues: list[Issue]) -> dict[str, int]:
    eval_dir = SWARM_ROOT / "tests" / "llm_eval"
    with tempfile.TemporaryDirectory(prefix="qgc-swarm-assets-") as temp_dir:
        temp_root = Path(temp_dir)
        generated_schema = temp_root / "tools_schema.json"
        command = [
            sys.executable,
            str(eval_dir / "export_tools.py"),
            str(REPO_ROOT / "src" / "Swarm" / "SwarmAIAgent.cc"),
            str(generated_schema),
        ]
        result = subprocess.run(command, capture_output=True, text=True, check=False, timeout=30)
        if result.returncode != 0:
            issues.append(Issue("error", "swarm_ext/tests/llm_eval", result.stderr.strip()))
        else:
            expected_schema = _load_json(eval_dir / "tools_schema.json", issues)
            actual_schema = _load_json(generated_schema, issues)
            if actual_schema != expected_schema:
                issues.append(
                    Issue(
                        "error", "swarm_ext/tests/llm_eval/tools_schema.json", "工具 Schema 已过期"
                    )
                )

        generated_one = temp_root / "dataset_one.jsonl"
        generated_two = temp_root / "dataset_two.jsonl"
        for output in (generated_one, generated_two):
            result = subprocess.run(
                [sys.executable, str(eval_dir / "generate_dataset.py"), str(output)],
                capture_output=True,
                text=True,
                check=False,
                timeout=30,
            )
            if result.returncode != 0:
                issues.append(Issue("error", "swarm_ext/tests/llm_eval", result.stderr.strip()))
                return {"llm_samples": 0}
        first = generated_one.read_bytes()
        second = generated_two.read_bytes()
        checked_in = (eval_dir / "dataset_v1.jsonl").read_bytes()
        if first != second:
            issues.append(Issue("error", "swarm_ext/tests/llm_eval", "数据集生成不确定"))
        if first != checked_in:
            issues.append(
                Issue("error", "swarm_ext/tests/llm_eval/dataset_v1.jsonl", "数据集已过期")
            )
        sample_count = len(generated_one.read_text(encoding="utf-8").splitlines())
        if sample_count < 500:
            issues.append(
                Issue("error", "swarm_ext/tests/llm_eval/dataset_v1.jsonl", "样本少于 500 条")
            )
    return {"llm_samples": sample_count}


def validate_xml(issues: list[Issue]) -> dict[str, int]:
    path = SWARM_ROOT / "mavlink" / "swarm_perception.xml"
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as exc:
        issues.append(Issue("error", str(path.relative_to(REPO_ROOT)), f"XML 解析失败：{exc}"))
        return {"mavlink_messages": 0}
    messages = root.findall(".//message")
    ids = [message.attrib.get("id") for message in messages]
    if len(ids) != len(set(ids)):
        issues.append(Issue("error", str(path.relative_to(REPO_ROOT)), "MAVLink message id 重复"))
    return {"mavlink_messages": len(messages)}


def run_validation() -> tuple[list[Issue], dict[str, int]]:
    issues: list[Issue] = []
    stats: dict[str, int] = {}
    for validator in (
        validate_scenarios,
        validate_templates,
        validate_source_inventory,
        validate_generated_contracts,
        validate_xml,
    ):
        stats.update(validator(issues))
    return issues, stats


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", type=Path, help="可选 JSON 报告路径")
    args = parser.parse_args()
    issues, stats = run_validation()
    payload = {
        "ok": not any(issue.severity == "error" for issue in issues),
        "stats": stats,
        "issues": [asdict(issue) for issue in issues],
    }
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(payload, ensure_ascii=False, indent=2))
    return 0 if payload["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
