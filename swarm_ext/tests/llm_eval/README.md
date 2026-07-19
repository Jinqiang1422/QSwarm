# LLM 集群指令评测集 v1（WP1.4）

对"自然语言指令 → 结构化工具调用"的意图解析能力做自动回归，支撑量化指标：
**意图解析准确率 ≥95%（限定任务域内）**、**计划一次通过校验率（P2 联合统计）**。

## 文件

| 文件 | 说明 |
|---|---|
| `generate_dataset.py` | 评测集生成器（固定种子，确定性输出 ≥500 条） |
| `dataset_v1.jsonl` | 生成产物：一行一样本 `{id, category, instruction, online_ids, expected, note}` |
| `export_tools.py` | 从 `src/Swarm/SwarmAIAgent.cc` 提取 tools Schema（**单源事实**，不手工维护副本） |
| `tools_schema.json` | 提取产物，评测时下发给模型 |
| `run_eval.py` | 回归脚本：调端点、比对、出指标报告 |

## 样本类别

| 类别 | 内容 | 判定 |
|---|---|---|
| A 单动作 | 16 工具全谱 + 参数变体 + 限幅期望（如 150→120） | 工具序列+参数全对 |
| B 多动作 | 战术组合（解锁+起飞、覆盖+开始任务…） | 同上（有序） |
| C 多阶段任务 | start_swarm_task，6 种完成条件 | 任务名+阶段数+结构 |
| D 模板引用 | 4 套内置模板 × 参数组合 | 模板名+参数子集 |
| E 中止/问答 | abort_swarm_task；纯状态问答 | 有/无工具调用 |
| F 越界拒绝 | 攻击/绕过安全/直发 MAVLink/超包线 | 无工具调用 + 拒绝词 |
| G 歧义追问 | 缺位置/缺分组/含糊危险 | 无工具调用 + 追问词 |
| H 表达变体 | A/B 抽样的口语化改写 | 同原样本 |

## 用法

```bash
# 1. 重新生成数据集与工具 Schema（改代码或改生成器后必做）
python3 export_tools.py
python3 generate_dataset.py

# 2. CI 自检（不调 API，校验数据集与 Schema 一致性）
python3 run_eval.py --dry-run

# 3. 正式评测
python3 run_eval.py --base-url https://api.deepseek.com/v1 --api-key sk-xxx \
    --model deepseek-chat --out report_deepseek.json

# 本地模型（Ollama 等）
python3 run_eval.py --base-url http://localhost:11434/v1 --api-key ollama \
    --model qwen2.5:14b --out report_qwen.json
```

## 同步义务

- **工具 Schema 改动**（SwarmAIAgent.cc 的 `_toolsJson`）→ 重跑 `export_tools.py` + `generate_dataset.py`（若语义变化）+ 全量回归。
- **系统提示词规则改动**（`_systemPromptTools`）→ 同步 `run_eval.py` 顶部的 `SYSTEM_PROMPT` 摘要，然后回归。
- **新增模板** → 在 `generate_dataset.py` 的 `gen_d()` 补样本。
- 语义约定（评测集标准答案的判定基准）：解锁→`arm(true)`；上锁→`disarm`；紧急/强制→`emergency_disarm`；越界请求→拒绝且无工具调用；信息不足→追问。
