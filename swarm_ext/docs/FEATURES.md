# 无人机集群指挥控制平台 — 功能总结

> 基于开源 QGroundControl 的「大模型 + 无人机集群」指挥控制平台
> 快照日期:2026-07-23 · 分支 `feature/swarm-extension` · 30 commit

---

## 一、软件定位

在开源 **QGroundControl**(master / v5.x)之上二次开发的**「大模型 + 无人机集群」指挥控制平台**。

核心设计原则:
- **大模型不直接控飞机** — LLM 只产出工具调用 → 校验闸门 → 下发,校验层与 LLM 进程/代码路径严格分离
- **QGC 是壳,不是全部** — 对上游净侵入仅 **9 文件 76 行**,能力集中在 `src/Swarm/`(21 模块 6953 行)+ 3 个 QML 面板(1061 行)
- **自主等级可分级** — L1 仅建议 / L2 监督执行 / L3 限定域自主

## 二、开发历程

```
下载 QGC → 合并 KIMI 扩展(7 commit) → P1 编译打通 → 评测基线 → CI → SITL 诊断 → P2 测试加固
         └ 一路修掉真实 bug:4 类 Swarm 编译错误 / 防注入失效 / 评测判定 4 缺陷 / Docker 代理 / ICU73
```

| 阶段 | 成果 |
|---|---|
| 合并 | QGC master + KIMI 集群扩展(src/Swarm 21 模块 + 3 QML + swarm_ext 资产) |
| P1 ① 编译 | 官方 Docker 打通整机编译,产物 633M,Swarm 全编入 |
| P1 ② 评测 | Kimi-K2.6 真实意图解析 88%,评测判定 4 缺陷全诊断+校准 |
| P1 ④ CI | 4 作业接入 .github/workflows |
| P1 ③ SITL | 诊断受阻于服务器仿真环境(GL/图形),已记录待续 |
| P2 | 约束/预演/拍卖三模块测试 8→27 slot,29 PASS |

---

## 三、当前软件功能

### 🎯 核心枢纽 — SwarmManager
多机实时监控(复用 MultiVehicleManager)、集群批量指令(毫秒交错下发防链路拥塞)、**统一动作管线**(`parseAndValidate` + `executeActions`,大模型 / 状态机 / 人工共用同一套)。向 QML 暴露 15 项能力(vehicles / selectedVehicles / formation / missionPlanner / aiAgent / taskController / taskLibrary / autonomyLevel / auditDir / copModel / perception / alerts / situationOneLiner …)。

### 域① 大模型集群指挥

| 能力 | 实现 | 状态 |
|---|---|---|
| 自然语言指挥 | SwarmAIAgent:NL → **17 工具 Function Calling** → 校验 → 下发 | 评测 88% ✅ |
| 双模部署 | OpenAI 兼容端点(DeepSeek / Kimi / 通义 云端 · Ollama 离线) | ✅ |
| 多候选方案 | propose_candidates:2~3 候选 + 对比卡片 + 选择/放弃 | I 类 100% ✅ |
| 防注入 | SwarmInputFilter:22 注入模式 + 三级判定(clean/suspicious/blocked) | 单测 ✅ |
| 自主分级 | SwarmAutonomy:L1 仅建议 / L2 监督 / L3 限定域自主 | 单测 ✅ |
| 审计追溯 | SwarmAuditLogger:指令→推理→校验→下发→反馈 全链 JSONL 落盘 | ✅ |

**17 个 LLM 工具**:
- 集群指令:`arm` `takeoff` `land` `rtl` `pause` `disarm` `emergency_disarm` `start_mission` `set_selection` `clear_missions`
- 编队:`start_formation` `stop_formation`
- 区域规划:`plan_coverage`(支持 sequential / auction 分配)
- 任务编排:`start_swarm_task` `abort_swarm_task` `run_task_template`
- 多候选:`propose_candidates`

### 域② 多机感知态势汇聚(P3)

- **SwarmVideoHub** 多路视频汇聚 · **SwarmPerceptionBridge** 感知接收(MAVLink TUNNEL,4 条自定义消息)· **SwarmTrackFusion** 多源航迹融合 · **SwarmCOPModel** 共同态势图 · **SwarmSituationDigest** 态势摘要(周期送 LLM)· **SwarmEventDetector** 异常事件检测告警 · **SwarmTunnelProtocol** 隧道协议解析
- **独立感知服务**(Python,不侵入 QGC 构建):YOLO 类检测 + 多目标跟踪 + 像素↔地理坐标转换,经 UDP/TUNNEL 回传结构化航迹

### 域③ 人机协同任务规划

| 能力 | 实现 | 状态 |
|---|---|---|
| 区域覆盖规划 | SwarmMissionPlanner:条带划分 + 割草机航线,与人工 Plan 同管线 | ✅ |
| 编队飞行 | FormationController:楔形 / 纵队 / 横队 / 环形,长机-僚机 | ✅ |
| 多阶段任务 | SwarmTaskController:状态机,监控 MISSION_CURRENT 自动推进 | ✅ |
| 模板库 | SwarmTaskLibrary:**9 套模板**(区域搜索/编队巡逻/扇形扫掠/平行网格/接力值守/会合集结/快速出动/编队巡航/全员返航) | ✅ |
| 约束校验闸门 | SwarmConstraintChecker:**10 规则**(空域围栏/航程电量/去冲突/链路覆盖/载荷匹配),error 阻断 + warning 提示 | 单测加固 ✅ |
| 仿真预演 | SwarmRehearsalEstimator:解析式时长/里程/电量裕度估算 | 单测 ✅ |
| 拍卖分配 | SwarmTaskAllocator:Bertsekas auction,ε-最优,反转场景转场里程降 95% | 单测 ✅ |

### 🖥️ 人机界面(FlyView 集成)

- **SwarmControlPanel** — 集群监控 / 批量指令 / 编队 / 任务规划
- **SwarmAIPanel** — AI 对话 / 计划确认 / 任务状态机 / 模板库 / 候选对比 / 参数编辑 / 时延与围栏显示
- **SwarmSituationLayer** — 地图态势叠加(目标航迹类别图标 + 置信度环 / 威胁区 / 覆盖热力,数据来自 COP)

---

## 四、架构分层(L0 → L5)

| 层 | 组件 | 状态 |
|---|---|---|
| L5 人机交互 | 3 QML 面板 + 态势图层 | ✅ |
| L4 认知 | Function Calling 意图解析 + 态势上下文注入 | ✅ |
| L3 规划 | 任务分解 / 时序编排 / 约束引擎 / 多候选 / 拍卖分配 | ✅ |
| L2 融合 | 遥测汇聚 / 航迹融合 / 事件检测 / COP | ✅(代码就绪) |
| L1 链路 | LinkManager / MultiVehicleManager 复用 + 自定义 MAVLink | ✅ |
| L0 平台 | PX4 SITL 场景库(6 场景) | ⚠️ 仿真环境受阻 |

---

## 五、验证状态与遗留

**已验证**
- 整机编译:官方 Docker 打通,产物 633M,Swarm 21 模块符号 + 3 QML 全编入 ✅
- 意图解析:559 条评测集,校准后 88%(安全类拒绝/追问/态势 100%) ✅
- 单元测试:**29 PASS / 0 FAIL**(P1 自主分级/防注入/审计 + P2 约束/预演/拍卖) ✅
- CI:4 作业(cpp-tests / llm-eval-sanity / scenario-sanity / perception-service)守护回归 ✅
- 运行时:进程启动、SwarmManager 初始化、QML 加载全程零 Swarm 报错 ✅

**遗留 / 待续**
- SITL 8 机实测:服务器仿真环境 GL/图形受阻(需 Xvfb+软件 GL 或换机)
- GUI 可视化:同源(headless 服务器无正常显示环境)
- P2 量化验证:约束一次通过率 / 规划耗时对比(依赖运行环境)
- 务实裁剪功能:围栏可视化编辑 / Plan 视图深度互通 / SSE 流式输出 / 载荷建模

---

## 六、辅助资产(swarm_ext/)

| 类别 | 内容 |
|---|---|
| docs/ | 二次开发方案 / 大模型接口方案 / 整体开发计划 / P1~P3 阶段说明 / 编译诊断 / 评测基线 |
| tests/cpp/ | 3 组 Qt Test 单元测试(P1/P2/P3 模块),CI 编译运行 |
| tests/llm_eval/ | 559 条评测集 + 生成器 + 回归脚本 + 校准重判工具(recalc.py) |
| sim/ | 6 个 PX4 SITL 场景 + MAVLink 场景驱动器 |
| services/perception/ | Python 感知服务(YOLO 检测 + 多目标跟踪) |
| mavlink/ | 4 条自定义感知/航迹 MAVLink 消息定义 |
| templates/ | 9 套任务模板 JSON |
| ci/ | P1 CI 参考(已接入 .github/workflows/swarm-p1-ci.yml) |

---

**一句话总结**:一个已编译可运行、经量化验证(意图解析 88%)、CI 守护(29 单测)的无人机集群指控平台 —— 覆盖「大模型自然语言指挥(17 工具 + 三道安全闸门)+ 多机感知态势汇聚 + 人机协同规划(约束校验 + 预演 + 拍卖分配)」三大功能域,核心代码 8000 余行,以 <5% 侵入嫁接在 QGroundControl 之上。
