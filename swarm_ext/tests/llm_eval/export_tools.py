#!/usr/bin/env python3
# ruff: noqa: RUF001, RUF002, RUF003
"""
export_tools.py — 从 SwarmAIAgent.cc 提取 OpenAI tools 定义 → tools_schema.json

单源事实原则：评测集回归使用的工具 Schema 必须与 QGC 内运行的代码一致，
因此不手工维护副本，而是从 C++ 源码中的原始 JSON 字符串自动提取。

用法：python3 export_tools.py [SwarmAIAgent.cc 路径] [输出路径]
"""

import json
import re
import sys
from pathlib import Path

# 合并进 QGC 后本脚本位于 swarm_ext/tests/llm_eval/,src/Swarm 在仓库根(上 3 级)
DEFAULT_SRC = Path(__file__).resolve().parents[3] / "src" / "Swarm" / "SwarmAIAgent.cc"
DEFAULT_OUT = Path(__file__).resolve().parent / "tools_schema.json"

# 两段式提取：先定位 fn(QStringLiteral("name"), QStringLiteral("desc"), params(R"( 起始，
# 再文本搜索 raw string 结束符 )"（工具 Schema 的 JSON 内容中不含该序列，安全）。
# description 支持 clang-format 拆分后的相邻 C++ 字符串字面量。
CPP_STRING = r'"(?:[^"\\]|\\.)*"'
FN_RE = re.compile(
    r'fn\(\s*QStringLiteral\(\s*"(?P<name>(?:[^"\\]|\\.)*)"\s*\)\s*,\s*'
    rf"QStringLiteral\(\s*(?P<desc>(?:{CPP_STRING}\s*)+)\)\s*,\s*"
    r'(?P<tail>params\(R"\(|noParams)',
    re.DOTALL,
)


def extract(src_path: Path) -> list:
    text = src_path.read_text(encoding="utf-8")
    tools = []
    for m in FN_RE.finditer(text):
        name, tail = m.group("name"), m.group("tail")
        desc = "".join(json.loads(item) for item in re.findall(CPP_STRING, m.group("desc")))
        if tail == "noParams":
            parameters = {"type": "object", "properties": {}}
        else:
            end = text.find(')"', m.end())  # raw string 结束符
            if end < 0:
                raise SystemExit(f"[export_tools] 找不到 raw string 结束符：{name}")
            try:
                parameters = json.loads(text[m.end() : end])
            except json.JSONDecodeError as e:
                raise SystemExit(f"[export_tools] {name} 参数 Schema 非法 JSON：{e}") from e
        tools.append(
            {
                "type": "function",
                "function": {"name": name, "description": desc, "parameters": parameters},
            }
        )
    declared_count = len(re.findall(r"\bfn\(\s*QStringLiteral\(", text))
    if len(tools) != declared_count:
        raise SystemExit(
            f"[export_tools] 提取到 {len(tools)}/{declared_count} 个工具，C++ 声明格式可能未被解析"
        )
    return tools


def main() -> None:
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SRC
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_OUT
    tools = extract(src)
    if len(tools) < 10:
        raise SystemExit(f"[export_tools] 仅提取到 {len(tools)} 个工具，疑似解析失败")
    out.write_text(json.dumps(tools, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"[export_tools] 提取 {len(tools)} 个工具 → {out}")
    for t in tools:
        print(f"  - {t['function']['name']}")


if __name__ == "__main__":
    main()
