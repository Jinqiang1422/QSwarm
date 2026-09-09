# 质量门禁治理与缺陷回归（2026-09-09）

本轮接续[上一轮验证记录](2026-09-09-quality-gates-followup.md)，起点为 `8a7f8711f`。
已提交 `f442bf5ca`（Python 工具与模拟器）、`fe54371f2`（检查入口）、`4878030a1`（图像与云台界面）。
本轮修复已完成验证；全仓库 lint 仍未通过。

## 已修复问题

| 范围 | 原有问题与修复行为 |
| --- | --- |
| 图像提供器 | 请求解引用可空的尺寸输出指针；非法飞机 ID 在转为 8 位整数后可能命中另一架飞机。现在先验证范围，再查找图像；无效请求返回占位图并正确报告尺寸，允许空尺寸指针。构造器只提供实际支持的 Image 类型。 |
| APM 云台 | 删除 RC 通道控件上未使用的重复 ID，保留伺服通道控件及其引用。Qt 6 检出的重复 ID 错误消失。 |
| MAVLink 模拟器 | 已复现原版解锁后电量变成 `99.99`，发送系统状态时编码器报 `required argument is not an integer`。现在内部保留小数电量，发送时转换为整数；耗电量按时间步长计算，拒绝未连接调用及非法频率、时间步长。 |
| XML 生成器 | 缺失 include、消息 ID/名称或实例字段名称会产生异常或不完整结果。现在在生成输出前拒绝这些输入，并校验 MAVLink/TS 根元素。 |
| Python 类型与依赖 | 返回已验证的 JSON 对象供后续读取，保护空输出流，使用类型检查器可识别的 Windows 平台条件；为 Pyright 配齐分析依赖，把真实 MAVLink 编码测试加入 CI 的 test 依赖并同步锁文件。 |
| 文件选择 | 分析器支持多个文件/目录并去重；默认扫描包含分支提交、暂存、未暂存和未跟踪文件；Git 失败及非法路径明确失败。pre-commit 将实际待检文件传入分析器。 |
| 静态分析结果 | Clazy 缺工具或编译数据库时失败，识别退出码为零的诊断和无诊断的非零退出；Clang-Tidy 保留诊断文本；qmllint 要求 Qt 6，并使用 SDK 默认导入目录和构建生成的 QML 模块。 |
| 门禁配置 | Hadolint 覆盖实际 Dockerfile；Vale 同步与检查复用同版本环境并使用项目配置；修正 CMake 格式配置项、ShellCheck trap 误报和固定等待检查的注释误报；生产空指针门禁排除其自身的故意违规测试夹具。 |

## 验证结果

证据目录：`test_results/2026-09-09-quality-gate-remediation/`。
构建使用现有 `qgc-ubuntu-docker:latest`，Qt 6.11.1、GCC 13，沿用仓库 CI 构建/测试入口。
Python 3.10.20 和 3.12.14 环境均按 `tools/uv.lock` 同步 scripts、test 依赖。

| 检查 | 最终结果 | 证据 |
| --- | --- | --- |
| Debug 构建 | 通过 | `build-final/` |
| Release 构建及测试接口剔除 | 通过 | `release-final/` |
| Unit | 184/184 通过，排除 Flaky、Network | `unit-final/`、`unit-final-LastTest.log` |
| 图像边界回归 | 通过，覆盖空尺寸指针、异常 URL、飞机 ID 溢出、多机隔离及翻转 | `image-provider-ctest.log` |
| 界面集成 | APMVehicleConfigUITest、TopLevelViewsTest 两个目标通过 | `ui-ctest.log`、`ui-LastTest.log` |
| Python 工具与 CI 脚本 | 两个 Python 版本各 928/928 通过，无跳过 | `python310-final.log`、`python312-final.log` |
| 原版缺陷复现 | 原版模拟器解锁后的真实 MAVLink 编码失败已复现 | `simulator-before-fix.log` |
| 类型检查 | 全范围 0 错误；保留 2 个警告，涉及动态导出列表和 GDB 专用模块 | `pyright-final.json`；完整 Pyright 钩子通过 |
| QML | Qt 6 实际检查 445 个 src 文件，0 阻断错误，命令成功 | `qmllint-qt6-final.log` |
| 修改范围格式 | Ruff、Python/C++ 格式、CMake 格式及 CMake lint 均通过 | `changed-format-checks.json` |
| 依赖锁文件 | 漂移检查通过，已包含在两个版本的 928 项测试中 | `uv-lock-check.log` |
| 全仓库 lint | 23 项通过、19 项失败，无跳过 | `just-lint-all.log`、`full-lint-summary.json` |

QML 成功依据现有 `.qmllint.ini` 的阈值，不代表无告警：最终日志仍包含 6342 条 Warning。
界面集成目标内仍有 1 个既有跳过：ArduSub Tuning 参数与 MockLink 不匹配。
本轮未重跑全部 61 个 Integration 目标、全部 SITL 场景或真实飞行器验收。

## 全库门禁的变化与剩余工作

上一轮为 20 项通过、21 项失败、1 项跳过。本轮恢复了 Pyright、ShellCheck、Vale 和固定等待检查。
Hadolint 从漏检变为实际执行并报告问题；Clazy 从缺环境时返回成功改为明确失败。
因此不能仅用失败项数量变化衡量软件缺陷数量。

| 剩余类别 | 需要处理的内容 |
| --- | --- |
| 文件和格式规范 | JSON、脚本可执行标记、行尾、C++/Python/CMake 格式，以及 YAML、Markdown、拼写和 CMake lint 的既有不合规项。 |
| 构建与 CI 检查 | Dockerfile 检出 WORKDIR、软件包版本、管道错误传播等问题；zizmor 的 Actions 检查仍有待处理项。 |
| C++ 静态分析环境 | 当前完整门禁所在宿主机缺少匹配的工具/编译环境；Clang-Tidy、Clazy 尚不能声明实际全量分析通过。 |
| QML 宿主机入口 | 宿主机 qmllint 是旧版，门禁明确失败；同一源码在构建容器的 Qt 6 环境已完成 445 文件检查。 |
| 运行时防御 | 仍有 73 处生产断言命中及其他模块的 Vehicle/Fact 空指针门禁问题。需按路径审查断言副作用、文件读取失败和参数缺失，再补充对应回归；命中数不等同于已确认 bug 数。 |

优先完成 C++ 分析环境及运行时防御检查，再按模块处理规范问题，机械格式修改单独提交。
QML 和 Vale 的现有告警阈值未降低；后续应逐步清理告警，不能把当前命令成功视作全部问题消失。

完整 lint 在专用工作树执行，每次执行前清除该工作树上一次钩子的未暂存自动格式修改，
再同步本轮源码。自动修改未带回主工作区。28 个实现文件的哈希保存在 `source-manifest.json`。
用户原有三份文档的 SHA-256 与本轮开始前一致，未跟踪资料也未纳入提交。
