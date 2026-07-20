# P1 首战:官方 Docker 打通 QGC + Swarm 整机编译 — 设计文档

- 日期:2026-07-19
- 分支:`feature/swarm-extension`
- 阶段:P1 工程硬化与安全底座 —— 遗留项第 1「QGC 整机编译」
- 状态:已评审通过,待实施

## 1. 背景

KIMI 生成的集群扩展(Swarm)已逐处合并进 QGroundControl(7 次 commit,核心侵入 6 文件 46 行,其余为新增)。P1 的 C++/QML/评测/场景/CI **代码已全部就绪并静态自洽**,但从未在真实 Qt6 环境编译运行过。本机为 Ubuntu 20.04(cmake 3.16、gcc 8/9、无 Qt6),不满足 QGC master 要求(cmake ≥3.25、C++20、Qt6.8+),因此第一战是"打通整机编译",它是评测基线、SITL 实测、CI 等所有后续 P1 验证的前提。

## 2. 目标与成功标准

用 QGC 官方 Docker 构建链,把合并了 Swarm 的 QGC 编译通过。

**成功标准:**
1. cmake 配置 + 构建全程无 error,产出 QGroundControl 可执行文件 / AppImage。
2. `src/Swarm/` 21 个模块全部编入主程序目标,`SwarmManager` 单例符号链接成功。
3. 3 个 Swarm QML(SwarmControlPanel / SwarmAIPanel / SwarmSituationLayer)注册进 FlyView QML 模块,无未解析符号 / QML 类型缺失。
4. 留存"编译通过"证据:构建日志尾部 + 产物路径 + `nm`/符号抽查。

## 3. 构建方案(方案 A:官方 Docker)

- 命令:`deploy/docker/run-docker.sh ubuntu Debug`
  - variant = `ubuntu`(基于 ubuntu:24.04,容器内自动装 Qt6.x + cmake≥3.25 + gcc-12,处理本机工具链过旧问题)
  - build-type = `Debug`(便于后续定位崩溃;首战优先 Debug,Release 后续再出)
- 仓库挂载进容器,构建产物写回宿主 build 目录(由 run-docker.sh / BUILD_DIR 决定)。
- 在当前 `feature/swarm-extension` 分支上构建。

## 4. 代理配置

Docker build/run 阶段需联网(apt、aqt 装 Qt、uv、vcpkg 依赖)。本机境外流量走代理(gost HTTP `127.0.0.1:8118` → socks5 VPS 出口 `10809`)。

- 实施前先做**代理通路小实验**:在容器内 `curl` 一个境外地址验证可达,再启动完整构建。
- 方式候选(实施时择优):Docker client `~/.docker/config.json` 的 `proxies` 段;或 `--build-arg http_proxy/https_proxy` + host 网络访问宿主 `127.0.0.1:8118`;容器访问宿主用 `host.docker.internal` 或 `--network host`。
- 不使用系统默认失效的 10808;VPS 出口 10809 已验证可用。

## 5. 长任务执行

首次构建耗时长(拉基础镜像 + 装 Qt + 下 vcpkg + 全量编译,预计 30–60min+)。按用户习惯放 **tmux 手动跑**;Claude 负责给命令、盯日志、定位并修复编译错误。

## 6. 编译错误修复循环(核心工作)

KIMI 代码已静态自洽,但首次真实编译大概率暴露细节问题(Qt6 头文件/API 细节、`QML_ELEMENT` 注册、include 顺序、moc、pre-commit 的 clang-tidy/clang-format 等)。

- 循环:编译报错 → 定位到具体 Swarm 文件/行 → 最小化修复 → 增量重编 → 直至通过。
- **每修复一类错误单独 commit**(延续"每处 commit"要求),message 说明修了什么、为何。
- 只改 Swarm 扩展代码(`src/Swarm/`、`src/FlyView/Swarm*.qml` 及 6 处集成点);非必要不动 QGC 上游代码。若上游集成点确有问题,单独标注说明。

## 7. Swarm 集成验证点

编译通过后确认:
- `add_subdirectory(Swarm)` 生效,Swarm 源文件出现在构建的编译单元中。
- `SwarmManager::instance()` 等符号存在于产物(`nm` 抽查)。
- QML 模块清单含 3 个 Swarm 面板类型。
- 应用可加载(至少启动不崩、QML 引擎无 Swarm 相关报错;GUI 实跑留待运行验证阶段)。

## 8. 交付物

- 可运行的 QGC 构建产物 + 编译通过日志(存档路径记录在 swarm_ext/docs)。
- 编译错误修复的一组 commit。
- 更新 `swarm_ext/docs`(或本 spec 附记)标记 P1 遗留第 1 项「整机编译」完成。

## 9. 范围与非目标

- **范围内**:仅"编译通过"及为此所需的 Swarm 代码修复、Docker/代理配置。
- **非目标**:GUI 实跑验证(运行验证阶段做)、LLM 评测基线首测(P1 第 2 战)、SITL 8 机实测(P1 第 3 战)、CI 接入 .github/workflows(P1 后续)。这些依赖本战成功后逐步展开。

## 10. 风险与应对

| 风险 | 应对 |
|---|---|
| Docker 拉镜像/装 Qt 因代理卡住 | 先做代理小实验;必要时改 socks5 10809 或分步预热镜像 |
| vcpkg 依赖下载慢/失败 | 走代理 + 重试;记录失败包单独处理 |
| Swarm 代码 Qt6 API 细节编译错误 | 逐类最小化修复并 commit;只动扩展代码 |
| 首次构建过长导致 session 中断 | tmux 手动跑,Claude 仅调试与检查进度 |
| pre-commit(clang-tidy 等)门禁报错 | 编译打通优先;门禁问题在编译通过后按 AGENTS.md 规范单独收敛 |
