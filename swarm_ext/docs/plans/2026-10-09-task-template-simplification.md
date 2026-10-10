# 任务模板库精简（9 → 6 套）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 内置任务模板从 9 套精简为 6 套。升级时把已经落盘的 3 套旧模板文件一次性改名退役。LLM 评测集同步出 v2，现行文档同步为 6 套。

**Architecture:** 代码改动有两处：
- 从 `SwarmTaskLibrary.cc` 删掉三套内置 JSON 常量，以及它们在落盘表、旧版摘要表和内置名单里的条目。
- 构造函数在 `_ensureDefaults()` 之前调用新函数 `_retireRemovedBuiltins()`。它读取模板目录里的版本标记 `.builtin-set-revision`；标记缺失或小于 3 时，把三个旧文件改名为 `.retired.bak`，再写入标记 3。

评测集方面：生成器把题目改写后输出 `dataset_v2.jsonl`，`run_eval.py` 和 `validate_assets.py` 都改为指向 v2。

**Tech Stack:** C++20 / Qt 6.11（QtCore、QtTest），Python 3（评测生成器与资产校验）。

**设计文档:** `swarm_ext/docs/specs/2026-10-09-task-template-simplification-design.md`（下称「设计」）。

---

## 执行约定（先读）

- **构建和测试只能在容器里跑。** 宿主没有 Qt，也没有 just。Task 0 会检查或重建两个脚本：
  - `/tmp/qgc_build.sh`：增量编译 `QGroundControl` 目标。
  - `/tmp/qgc_ctest.sh <ctest 参数>`：在容器里运行 `ctest --output-on-failure <参数>`。容器的 `HOME` 设在
    `/tmp/qgc-test-home`，用完即丢，所以每次 ctest 的模板目录都是全新的。
- **Python 脚本直接在宿主跑**（宿主 Python 3.8，`validate_assets.py` 和评测脚本都已验证可用）。
- **不要逐任务 `git commit`。** 用户习惯在全部完成后统一决定提交方式。每个任务以「验证通过」为检查点，Task 5 结束时再问用户。
- **格式化：**
  - C++ 用宿主的 `clang-format`（22.1.5，与 pre-commit 一致）。
  - Python 用 pre-commit 锁定的 ruff 0.15.20，路径 `/tmp/rv_ruff/x/ruff-0.15.20.data/scripts/ruff`。宿主 `ruff` 是 0.9.0，**不要用**。
- **代码约束：**
  - 用户可见文字一律用 `tr()`；不要用 `Q_ASSERT`。
  - 测试里不要用 `QTest::qWait(<n>)`，也不要用 `QTest::ignoreMessage`，改用 `ignoreLogMessage` / `expectLogMessage`。
- **正在运行的 SITL（tmux `sitl5`）和 GUI 容器 `qgc-swarm5-gui` 不要动。** GUI 用的是 `build-release/`，
  与本计划编译的 `build/` 互不影响。

## 文件结构

| 文件 | 动作 | 职责 |
|---|---|---|
| `src/Swarm/SwarmTaskLibrary.cc` | 修改 | 删 3 套内置模板；新增 `_retireRemovedBuiltins()`；构造函数调用它 |
| `src/Swarm/SwarmTaskLibrary.h` | 修改 | 头注释改为 6 套并说明退役；声明 `_retireRemovedBuiltins()` |
| `swarm_ext/templates/{扇形扫掠搜索,编队巡逻,会合集结}.json` | 删除 | 内置模板的仓库镜像，`validate_assets.py` 要求它与内嵌模板一致 |
| `test/Swarm/SwarmRegressionTest.h/.cc` | 修改 | 模板名单改为 6 套；新增退役测试 `removedBuiltinTemplatesAreRetiredOnce` |
| `swarm_ext/tests/llm_eval/generate_dataset.py` | 修改 | 题目改写，默认输出 v2 |
| `swarm_ext/tests/llm_eval/dataset_v2.jsonl` | 新建（生成） | v2 评测集 |
| `swarm_ext/tests/llm_eval/run_eval.py` | 修改 | 上下文模板名单改为 6 套；默认数据集改为 v2 |
| `swarm_ext/tests/llm_eval/README.md` | 修改 | 说明 v2 及其相对 v1 的变化 |
| `swarm_ext/tests/validate_assets.py` | 修改 | 数据集一致性检查改为比对 v2 |
| `swarm_ext/docs/用户使用手册.md` 等 7 份文档 | 修改 | 见 Task 4 |

`dataset_v1.jsonl`、`BASELINE_kimi_k26.md`、`reports/` 都**保持不变**。

---

### Task 0: 准备

**Files:** 无代码改动

- [ ] **Step 1: 建分支**

当前分支 `feat/synchronized-formation-legs`、`main`、`originJin/main` 都指向 `77105a441`，工作区只有不属于本计划的未跟踪文件（`test.md`、`.understand-anything/`），以及本计划的设计文档和计划文档。

```bash
cd /media/hello/data1/Jinqiang/QGC/qgroundcontrol
git rev-parse --short HEAD main          # 两行都应为 77105a441
git switch -c refactor/task-template-simplification main
```

- [ ] **Step 2: 确认辅助脚本存在（不存在则重建）**

```bash
ls -l /tmp/qgc_build.sh /tmp/qgc_ctest.sh
```

如果缺失，按下面的内容重建，再 `chmod +x` 两个文件：

```bash
cat > /tmp/qgc_build.sh <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
QGC=/media/hello/data1/Jinqiang/QGC/qgroundcontrol
docker run --rm -u 1000:1000 --entrypoint bash \
  -v "$QGC:/project/source" -v "$QGC/build:/project/build" \
  qgc-ubuntu-docker:latest -lc 'cmake --build /project/build --target QGroundControl --parallel 20'
EOF
cat > /tmp/qgc_ctest.sh <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
QGC=/media/hello/data1/Jinqiang/QGC/qgroundcontrol
docker run --rm -u 1000:1000 --entrypoint bash \
  -v "$QGC:/project/source" -v "$QGC/build:/project/build" \
  -e QT_QPA_PLATFORM=offscreen -e HOME=/tmp/qgc-test-home \
  qgc-ubuntu-docker:latest -c "cd /project/build && ctest --output-on-failure $*"
EOF
chmod +x /tmp/qgc_build.sh /tmp/qgc_ctest.sh
```

- [ ] **Step 3: 确认锁定版 ruff 存在（不存在则下载）**

```bash
/tmp/rv_ruff/x/ruff-0.15.20.data/scripts/ruff --version   # 期望 ruff 0.15.20
```

如果缺失（外网下载必须走代理）：

```bash
mkdir -p /tmp/rv_ruff && cd /tmp/rv_ruff
env -u HTTP_PROXY -u HTTPS_PROXY -u http_proxy -u https_proxy \
  proxychains4 -f /etc/proxychains4.conf \
  pip download ruff==0.15.20 --no-deps --only-binary=:all: --platform manylinux_2_17_x86_64 -d .
unzip -q -o ruff-0.15.20-*.whl -d x && chmod +x x/ruff-0.15.20.data/scripts/ruff
```

- [ ] **Step 4: 记录基线**

```bash
cd /media/hello/data1/Jinqiang/QGC/qgroundcontrol
python3 swarm_ext/tests/validate_assets.py | tail -9
```

期望：`"templates": 9`、`"template_stages": 33`、`"llm_samples": 559`、`"issues": []`。

---

### Task 1: 从代码中删除三套内置模板

**Files:**
- Modify: `src/Swarm/SwarmTaskLibrary.cc`（常量区 106–147、184–219、248–275 行；`_ensureDefaults` 落盘表与旧版摘要表；`_scan` 内置名单）
- Modify: `src/Swarm/SwarmTaskLibrary.h:7-8`（头注释）
- Delete: `swarm_ext/templates/扇形扫掠搜索.json`、`swarm_ext/templates/编队巡逻.json`、`swarm_ext/templates/会合集结.json`
- Test: `test/Swarm/SwarmRegressionTest.cc:477-485`（`allTemplatesUseRuntimeContracts` 的名单）

这是纯删除，没有新的红灯测试：保留的 6 套由已有的 `allTemplatesUseRuntimeContracts` 覆盖，「三套已不存在」由 Task 2 的退役测试覆盖。

- [ ] **Step 1: 测试名单改为 6 套**

把 `test/Swarm/SwarmRegressionTest.cc` 中 `allTemplatesUseRuntimeContracts` 开头的名单：

```cpp
    const QStringList names{
        QStringLiteral("区域搜索后返航"), QStringLiteral("编队巡航后返航"), QStringLiteral("快速出动待命"),
        QStringLiteral("全员返航降落"),   QStringLiteral("扇形扫掠搜索"),   QStringLiteral("平行网格扫掠"),
        QStringLiteral("编队巡逻"),       QStringLiteral("多机接力值守"),   QStringLiteral("会合集结")};
```

替换为：

```cpp
    const QStringList names{QStringLiteral("区域搜索后返航"), QStringLiteral("编队巡航后返航"),
                            QStringLiteral("快速出动待命"),   QStringLiteral("全员返航降落"),
                            QStringLiteral("平行网格扫掠"),   QStringLiteral("多机接力值守")};
```

（最终排版以 Step 6 的 clang-format 为准。）

- [ ] **Step 2: 删除三个内置 JSON 常量**

先核对行号（文件自 `77105a441` 起未改动过）：

```bash
cd /media/hello/data1/Jinqiang/QGC/qgroundcontrol
sed -n '106p;146p;184p;218p;248p;274p;276p' src/Swarm/SwarmTaskLibrary.cc
```

期望依次输出：

```text
const char kTplSectorSweep[] = R"TPL({
})TPL";
const char kTplFormationPatrol[] = R"TPL({
})TPL";
const char kTplRendezvous[] = R"TPL({
})TPL";
}  // namespace
```

完全一致才执行删除（sed 按原始行号一次处理，三段互不影响）：

```bash
sed -i -e '248,275d' -e '184,219d' -e '106,147d' src/Swarm/SwarmTaskLibrary.cc
grep -c '^const char kTpl' src/Swarm/SwarmTaskLibrary.cc      # 期望 6
grep -n 'kTplSectorSweep\|kTplFormationPatrol\|kTplRendezvous' src/Swarm/SwarmTaskLibrary.cc   # 期望只剩落盘表里的 3 处引用
```

- [ ] **Step 3: 落盘表、旧版摘要表、内置名单去掉三套**

`_ensureDefaults()` 里的 `defaults[]` 初始化改为：

```cpp
    } defaults[] = {
        {"区域搜索后返航.json", kTplSearchReturn}, {"编队巡航后返航.json", kTplFormationCruise},
        {"快速出动待命.json", kTplQuickLaunch},    {"全员返航降落.json", kTplAllReturn},
        {"平行网格扫掠.json", kTplGridSweep},      {"多机接力值守.json", kTplRelayWatch},
    };
```

`legacyDigests` 删除 `扇形扫掠搜索`、`编队巡逻`、`会合集结` 三项，改为：

```cpp
    static const QHash<QString, QByteArray> legacyDigests{
        {QStringLiteral("区域搜索后返航"),
         QByteArray("3d9f90e088fe0366bdd193ddcfe06258be6ee2359f61a1d0e30d4ff649036a13")},
        {QStringLiteral("编队巡航后返航"),
         QByteArray("c0fb1568a4580ce1dbaa07d6403781f3e4ecb706fbeace61f2c187d6a35ee1ce")},
        {QStringLiteral("快速出动待命"),
         QByteArray("c83e3f4e6e51dbe670e739f1f26bc81a641bac3488e3547dacbcbd6ab71395a0")},
        {QStringLiteral("全员返航降落"),
         QByteArray("7d4e3230068f6960454f9d91e57c12273c542edf0f2b1c09ef8c3014e5ce7e4a")},
        {QStringLiteral("平行网格扫掠"),
         QByteArray("a820306d4bff8db36644d579e1e435f77fcdfa2d6727b42eabd87a4d90f99af6")},
        {QStringLiteral("多机接力值守"),
         QByteArray("06455cd6a63b7ce5e7a6b24b90a96403a766ca8069ba129db8c5ca050ec8ffea")}};
```

`_scan()` 开头的 `builtInNames` 改为：

```cpp
    static const QSet<QString> builtInNames = {
        QStringLiteral("区域搜索后返航"), QStringLiteral("编队巡航后返航"), QStringLiteral("快速出动待命"),
        QStringLiteral("全员返航降落"),   QStringLiteral("平行网格扫掠"),   QStringLiteral("多机接力值守")};
```

- [ ] **Step 4: 头注释改为 6 套**

`src/Swarm/SwarmTaskLibrary.h` 第 7–8 行：

```cpp
 *   - 内置模板：首次启动自动落盘 4 套（区域搜索后返航 / 编队巡航后返航 /
 *     快速出动待命 / 全员返航降落），落盘后与用户模板同等待遇、可编辑
```

改为：

```cpp
 *   - 内置模板：首次启动自动落盘 6 套（区域搜索后返航 / 平行网格扫掠 / 编队巡航后返航 /
 *     快速出动待命 / 全员返航降落 / 多机接力值守），落盘后与用户模板同等待遇、可编辑
```

- [ ] **Step 5: 删除仓库镜像并校验资产**

```bash
rm swarm_ext/templates/扇形扫掠搜索.json swarm_ext/templates/编队巡逻.json swarm_ext/templates/会合集结.json
python3 swarm_ext/tests/validate_assets.py | tail -9
```

期望：`"templates": 6`、`"template_stages": 18`、`"issues": []`。

- [ ] **Step 6: 格式化、编译、测试**

```bash
clang-format -i src/Swarm/SwarmTaskLibrary.cc src/Swarm/SwarmTaskLibrary.h test/Swarm/SwarmRegressionTest.cc
git diff --stat
/tmp/qgc_build.sh 2>&1 | tail -3
/tmp/qgc_ctest.sh -R SwarmRegressionTest
```

期望：编译成功，`SwarmRegressionTest` 1/1 通过（约 90 s）。`git diff --stat` 应只涉及上面 3 个源文件和 3 个被删除的 JSON。

---

### Task 2: 已落盘旧文件的一次性退役（TDD）

**Files:**
- Modify: `src/Swarm/SwarmTaskLibrary.h`（头注释补一行；私有段声明 `_retireRemovedBuiltins()`）
- Modify: `src/Swarm/SwarmTaskLibrary.cc`（include、构造函数、新函数）
- Test: `test/Swarm/SwarmRegressionTest.h`、`test/Swarm/SwarmRegressionTest.cc`

行为（设计 §3.2）：
- 读取模板目录下的标记 `.builtin-set-revision`。标记缺失或小于 3 时，把存在的 `扇形扫掠搜索.json`、`编队巡逻.json`、`会合集结.json` 一律改名为 `<文件名>.retired.bak`；如果该备份名已存在，改用 `<文件名>.retired-<yyyyMMddHHmmss>.bak`。
- 每退役一个文件，记一条审计日志 `task_library / template_retired`。
- 全部成功后写入标记 `3`。
- 改名或写标记失败时，通过 `libraryFeedback` 提示，并且不写标记，下次启动重试。

- [ ] **Step 1: 写失败测试**

`test/Swarm/SwarmRegressionTest.h`：在 `void legacyTemplateMigrationPreservesCustomEdits();` 下一行加

```cpp
    void removedBuiltinTemplatesAreRetiredOnce();
```

`test/Swarm/SwarmRegressionTest.cc`：
- include 区按字母序加入 `#include <QtCore/QDir>`，放在 `#include <QtCore/QFile>` 之前。
- 在 `legacyTemplateMigrationPreservesCustomEdits()` 的函数体之后，`allTemplatesUseRuntimeContracts()` 之前，加入下面的函数：

```cpp
void SwarmRegressionTest::removedBuiltinTemplatesAreRetiredOnce()
{
    const QString dir = _manager->taskLibrary()->templateDir();
    const QString marker = dir + QStringLiteral("/.builtin-set-revision");
    const QStringList removed{QStringLiteral("扇形扫掠搜索"), QStringLiteral("编队巡逻"), QStringLiteral("会合集结")};
    const auto cleanup = qScopeGuard([dir, removed] {
        for (const auto& name : removed) {
            for (const auto& fileName : QDir(dir).entryList({name + QStringLiteral(".json*")}, QDir::Files))
                QFile::remove(dir + QLatin1Char('/') + fileName);
        }
    });
    const auto readFile = [](const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const auto writeFile = [](const QString& path, const QByteArray& bytes) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    const QJsonArray stages{QJsonObject{{"name", QStringLiteral("返航")},
                                        {"actions", QJsonArray{QJsonObject{{"type", "rtl"}}}},
                                        {"complete_when", QJsonObject{{"kind", "all_disarmed"}}}}};
    const auto templateBytes = [&stages](const QString& name, const QString& description) {
        return QJsonDocument(QJsonObject{{"name", name}, {"description", description}, {"stages", stages}}).toJson();
    };
    const auto find = [](const SwarmTaskLibrary& library, const QString& name) {
        for (const auto& entry : library.templates()) {
            if (entry.toMap().value("name").toString() == name)
                return entry.toMap();
        }
        return QVariantMap();
    };

    // 全新目录也要写入标记（夹具在 init 中已构造过一次模板库）
    QCOMPARE(readFile(marker).trimmed(), QByteArray("3"));

    // 升级：没有标记时，三个旧内置模板文件一律退役，包括被操作员改过的那个
    QVERIFY(QFile::remove(marker));
    QHash<QString, QByteArray> originals;
    for (const auto& name : removed) {
        originals[name] = templateBytes(name, name == QStringLiteral("编队巡逻") ? QStringLiteral("operator edit")
                                                                                 : QStringLiteral("old built-in"));
        QVERIFY(writeFile(dir + QLatin1Char('/') + name + QStringLiteral(".json"), originals[name]));
    }
    {
        SwarmTaskLibrary upgraded(_manager.get());
        for (const auto& name : removed) {
            QVERIFY(!QFile::exists(dir + QLatin1Char('/') + name + QStringLiteral(".json")));
            QCOMPARE(readFile(dir + QLatin1Char('/') + name + QStringLiteral(".json.retired.bak")), originals[name]);
            QVERIFY(find(upgraded, name).isEmpty());
        }
        QStringList builtIns;
        for (const auto& entry : upgraded.templates()) {
            if (entry.toMap().value("builtIn").toBool())
                builtIns.append(entry.toMap().value("name").toString());
        }
        builtIns.sort();
        QStringList expected{QStringLiteral("区域搜索后返航"), QStringLiteral("平行网格扫掠"),
                             QStringLiteral("编队巡航后返航"), QStringLiteral("快速出动待命"),
                             QStringLiteral("全员返航降落"),   QStringLiteral("多机接力值守")};
        expected.sort();
        QCOMPARE(builtIns, expected);
        QCOMPARE(readFile(marker).trimmed(), QByteArray("3"));
    }

    // 标记已为 3：用户之后自建的同名模板不会再被退役
    ignoreLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("模板「会合集结」已保存")));
    QVERIFY(_manager->taskLibrary()->saveTemplate(QStringLiteral("会合集结"), QStringLiteral("operator rendezvous"),
                                                  stages, {}));
    const QString userPath = dir + QStringLiteral("/会合集结.json");
    const QByteArray userBytes = readFile(userPath);
    QVERIFY(!userBytes.isEmpty());
    {
        SwarmTaskLibrary rebuilt(_manager.get());
        QCOMPARE(readFile(userPath), userBytes);
        const auto user = find(rebuilt, QStringLiteral("会合集结"));
        QVERIFY(!user.isEmpty());
        QVERIFY(!user.value("builtIn").toBool());
    }

    // 同名 .retired.bak 已存在时改用带时间戳的备份名，不覆盖旧备份
    QVERIFY(QFile::remove(marker));
    {
        SwarmTaskLibrary upgradedAgain(_manager.get());
    }
    QVERIFY(!QFile::exists(userPath));
    QCOMPARE(readFile(userPath + QStringLiteral(".retired.bak")), originals[QStringLiteral("会合集结")]);
    const auto stamped = QDir(dir).entryList({QStringLiteral("会合集结.json.retired-*.bak")}, QDir::Files);
    QCOMPARE(stamped.size(), 1);
    QVERIFY(QRegularExpression(QStringLiteral("^会合集结\\.json\\.retired-\\d{14}\\.bak$"))
                .match(stamped.first())
                .hasMatch());
    QCOMPARE(readFile(dir + QLatin1Char('/') + stamped.first()), userBytes);
}
```

说明：
- 模板目录是这个测试类专用的 AppData（应用名 `QGroundControl_unittest_SwarmRegressionTest`），容器 HOME 每次 ctest 都是新的。即便如此，`cleanup` 仍会删掉本测试创建的所有 `<名称>.json*`，避免影响同类里的其他测试。
- 测试结束时标记为 3：最后一次构造模板库时已写入。
- 测试只通过公开接口（`templateDir()`、`templates()`、`saveTemplate()`）和文件系统观察行为。

- [ ] **Step 2: 编译并确认测试失败**

```bash
clang-format -i test/Swarm/SwarmRegressionTest.cc test/Swarm/SwarmRegressionTest.h
/tmp/qgc_build.sh 2>&1 | tail -3
/tmp/qgc_ctest.sh -R SwarmRegressionTest 2>&1 | grep -E 'FAIL!|Actual|Expected|tests passed' | head
```

期望：编译成功，`removedBuiltinTemplatesAreRetiredOnce` 失败在第一条 `QCOMPARE`（`Actual (readFile(marker).trimmed()): ""`，`Expected: "3"`）。如果失败点在别处，先查明原因再继续。

- [ ] **Step 3: 实现**

`src/Swarm/SwarmTaskLibrary.h`：
- 头注释接在 Task 1 改过的两行之后，补一行（「可编辑」后加分号）：

```cpp
 *   - 内置模板：首次启动自动落盘 6 套（区域搜索后返航 / 平行网格扫掠 / 编队巡航后返航 /
 *     快速出动待命 / 全员返航降落 / 多机接力值守），落盘后与用户模板同等待遇、可编辑；
 *     2026-10 移除的 3 套旧内置模板在升级时一次性改名为 *.retired.bak
```

- 私有段，在 `void _ensureDefaults();  ///< 内置模板首次落盘` 之前加：

```cpp
    void _retireRemovedBuiltins();  ///< 升级时一次性退役已移除的旧内置模板
```

`src/Swarm/SwarmTaskLibrary.cc`：
- include 区按字母序加入 `#include <QtCore/QDateTime>`，放在 `#include <QtCore/QCryptographicHash>` 之后。
- 构造函数在 `_ensureDefaults();` 之前加一行：

```cpp
    _retireRemovedBuiltins();
    _ensureDefaults();
    _scan();
```

- 在 `void SwarmTaskLibrary::_ensureDefaults()` 定义之前加入：

```cpp
void SwarmTaskLibrary::_retireRemovedBuiltins()
{
    // 2026-10 内置模板由 9 套精简为 6 套。升级后首次启动时，把已落盘的 3 套旧内置模板一律改名退役
    // （不论是否被编辑过）；写入标记后不再检查，此后同名文件按用户模板对待。
    static constexpr int kBuiltinSetRevision = 3;
    static const char* const kRetiredFiles[] = {"扇形扫掠搜索.json", "编队巡逻.json", "会合集结.json"};

    const QString markerPath = _templateDir + QStringLiteral("/.builtin-set-revision");
    QFile marker(markerPath);
    if (marker.open(QIODevice::ReadOnly) && marker.readAll().trimmed().toInt() >= kBuiltinSetRevision)
        return;
    marker.close();

    bool retiredAll = true;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss"));
    for (const char* retiredFile : kRetiredFiles) {
        const QString fileName = QString::fromUtf8(retiredFile);
        const QString path = _templateDir + QLatin1Char('/') + fileName;
        if (!QFile::exists(path))
            continue;
        QString backupName = fileName + QStringLiteral(".retired.bak");
        if (QFile::exists(_templateDir + QLatin1Char('/') + backupName))
            backupName = fileName + QStringLiteral(".retired-%1.bak").arg(stamp);
        if (!QFile::rename(path, _templateDir + QLatin1Char('/') + backupName)) {
            retiredAll = false;
            emit libraryFeedback(tr("旧模板退役失败：%1").arg(path));
            continue;
        }
        SwarmAuditLogger::instance()->log(QStringLiteral("task_library"), QStringLiteral("template_retired"),
                                          {{QStringLiteral("file"), fileName}, {QStringLiteral("backup"), backupName}});
    }
    if (!retiredAll)
        return;

    QSaveFile out(markerPath);
    const QByteArray bytes = QByteArray::number(kBuiltinSetRevision);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit())
        emit libraryFeedback(tr("模板版本标记写入失败：%1").arg(markerPath));
}
```

要点：
- `libraryFeedback` 已在构造函数里先于本调用连接到 `showAppMessage`，所以失败提示会显示在应用消息条上。
- 标记文件以 `.` 开头且不是 `*.json`，`_scan()` 不会列出它。
- 退役文件的扩展名不是 `.json`，也不会被列出。

- [ ] **Step 4: 编译并确认测试通过**

```bash
clang-format -i src/Swarm/SwarmTaskLibrary.cc src/Swarm/SwarmTaskLibrary.h
/tmp/qgc_build.sh 2>&1 | tail -3
/tmp/qgc_ctest.sh -R SwarmRegressionTest
```

期望：`SwarmRegressionTest` 1/1 通过，其中 `removedBuiltinTemplatesAreRetiredOnce` 为 PASS。

- [ ] **Step 5: 回看 diff**

```bash
git diff src/Swarm/SwarmTaskLibrary.cc src/Swarm/SwarmTaskLibrary.h test/Swarm/SwarmRegressionTest.h test/Swarm/SwarmRegressionTest.cc | less
grep -n 'Q_ASSERT\|qWait(' src/Swarm/SwarmTaskLibrary.cc test/Swarm/SwarmRegressionTest.cc   # 期望无新增
```

---

### Task 3: LLM 评测集 v2

**Files:**
- Modify: `swarm_ext/tests/llm_eval/generate_dataset.py`（文档串 4/10/16 行；`_task_entry` 之后加辅助函数；`gen_c` 末尾；`gen_d` 的「P2 新增模板」段；`main` 默认输出）
- Create（生成）: `swarm_ext/tests/llm_eval/dataset_v2.jsonl`
- Modify: `swarm_ext/tests/llm_eval/run_eval.py:124-134`、`:293`
- Modify: `swarm_ext/tests/validate_assets.py:407-417`
- Modify: `swarm_ext/tests/llm_eval/README.md`

题目映射见设计 §3.3。v1 编号在 v2 中会顺延，因为 4 道题从 D 类移到了 C 类，C 类排在 D 类之前。

- [ ] **Step 1: 生成器文档串**

`generate_dataset.py` 顶部文档串改三处：
- `generate_dataset.py — LLM 集群指令评测集 v1 生成器（WP1.4）` → `generate_dataset.py — LLM 集群指令评测集 v2 生成器（WP1.4）`
- `  D 模板引用（run_task_template，4 套内置模板 × 参数组合）` → `  D 模板引用（run_task_template，6 套内置模板 × 参数组合）`
- `用法：python3 generate_dataset.py [输出路径]   （默认 dataset_v1.jsonl）` → `用法：python3 generate_dataset.py [输出路径]   （默认 dataset_v2.jsonl）`

- [ ] **Step 2: C 类新增 4 题**

在 `_task_entry` 函数定义之后（`def gen_c()` 之前）加入辅助函数：

```python
def _sweep_stages(altitude, length, width, angles):
    """起飞 → 按各角度依次条带覆盖 → 返航（v1「扇形扫掠搜索」模板对应的自定义编排）。"""
    stages = [
        {
            "name": "起飞",
            "actions": [{"type": "arm", "arm": True}, {"type": "takeoff", "altitude": altitude}],
            "complete_when": {"kind": "all_airborne", "min_altitude": altitude - 5},
        }
    ]
    for angle in angles:
        stages.append(
            {
                "name": f"{angle}度扫掠",
                "actions": [
                    {"type": "plan_coverage", "length": length, "width": width, "angle": angle},
                    {"type": "start_mission"},
                ],
                "complete_when": {"kind": "all_mission_finished"},
            }
        )
    stages.append(
        {
            "name": "返航",
            "actions": [{"type": "rtl"}],
            "complete_when": {"kind": "all_disarmed"},
        }
    )
    return stages
```

在 `gen_c()` 末尾的 `return out` 之前（即「巡逻后搜索」那条 `out.append(...)` 之后）加入：

```python
    # --- v2：原「扇形扫掠搜索」「编队巡逻」模板题，模板移除后改为自定义编排 ---
    for instruction, altitude in (
        ("按0、60、120度三个角度对该区域依次扫掠搜索，完成后返航", 30),
        ("高度40米，对300×150米区域按0、60、120度三个角度依次扫掠，完成后返航", 40),
        ("对当前区域做三轮扫掠，航线角度依次为0度、60度、120度，扫完返航", 30),
    ):
        out.append(
            _task_entry(
                instruction,
                "三角度扫掠搜索",
                _sweep_stages(altitude, 300, 150, (0, 60, 120)),
                note="v2：原扇形扫掠搜索模板题",
            )
        )
    out.append(
        _task_entry(
            "1号长机楔形编队巡逻5分钟，然后换成纵队再巡逻3分钟，结束后返航",
            "编队巡逻换队形",
            [
                {
                    "name": "起飞",
                    "actions": [{"type": "arm", "arm": True}, {"type": "takeoff", "altitude": 40}],
                    "complete_when": {"kind": "all_airborne", "min_altitude": 35},
                },
                {
                    "name": "楔形编队",
                    "actions": [{"type": "start_formation", "leader_id": 1, "formation": "wedge"}],
                    "complete_when": {"kind": "formation_running"},
                },
                {
                    "name": "第一程巡逻",
                    "actions": [],
                    "complete_when": {"kind": "duration", "seconds": 300},
                },
                {
                    "name": "换纵队",
                    "actions": [{"type": "start_formation", "leader_id": 1, "formation": "column"}],
                    "complete_when": {"kind": "formation_running"},
                },
                {
                    "name": "第二程巡逻",
                    "actions": [],
                    "complete_when": {"kind": "duration", "seconds": 180},
                },
                {
                    "name": "返航",
                    "actions": [{"type": "stop_formation"}, {"type": "rtl"}],
                    "complete_when": {"kind": "all_disarmed"},
                },
            ],
            note="v2：原编队巡逻模板题",
        )
    )
```

- [ ] **Step 3: D 类改写**

`gen_d()` 中从 `        # --- P2 新增模板 ---` 到列表结尾 `    ]` 之前的整段（v1 的 E0162–E0172，共 11 条）替换为：

```python
        # --- P2 新增模板（v2：扇形扫掠搜索 / 编队巡逻 / 会合集结已移除）---
        _tpl_entry("平行网格扫掠模板，默认参数", "平行网格扫掠", {}),
        _tpl_entry("网格扫掠模板，区域500×400米", "平行网格扫掠", {"length": 500, "width": 400}),
        _tpl_entry("编队巡航后返航模板，默认参数", "编队巡航后返航", {}, note="v2：原编队巡逻模板默认题"),
        _tpl_entry(
            "多机接力值守模板，A组1到4号、B组5到8号",
            "多机接力值守",
            {"group_a_ids": [1, 2, 3, 4], "group_b_ids": [5, 6, 7, 8]},
        ),
        _tpl_entry("接力值守模板，每组值守10分钟", "多机接力值守", {"watch_seconds": 600}),
        _tpl_entry(
            "按编队巡航模板做一次环形编队会合集结，半径25米保持2分钟",
            "编队巡航后返航",
            {"formation": "circle", "spacing": 25, "cruise_seconds": 120},
            note="v2：会合集结并入编队巡航后返航",
        ),
        _tpl_entry(
            "按编队巡航模板做环形编队会合集结，默认参数",
            "编队巡航后返航",
            {"formation": "circle"},
            note="v2：会合集结并入编队巡航后返航",
        ),
```

`gen_i()` 中的「编队巡逻，给我两个方案选」「会合集结，出两个方案」（v1 E0253、E0259）**不改**，因为候选方案题只比工具名。

- [ ] **Step 4: 默认输出改为 v2**

`main()` 中的 `Path(__file__).parent / "dataset_v1.jsonl"` 改为 `Path(__file__).parent / "dataset_v2.jsonl"`。

- [ ] **Step 5: 生成 v2 并核对差异**

```bash
cd /media/hello/data1/Jinqiang/QGC/qgroundcontrol/swarm_ext/tests/llm_eval
python3 generate_dataset.py
git diff --exit-code dataset_v1.jsonl && echo "v1 unchanged"
python3 - <<'EOF'
import json
load = lambda p: [json.loads(line) for line in open(p, encoding="utf-8")]
v1, v2 = load("dataset_v1.jsonl"), load("dataset_v2.jsonl")
key = lambda e: (e["category"], e["instruction"], json.dumps(e["expected"], ensure_ascii=False, sort_keys=True))
a, b = {key(e) for e in v1}, {key(e) for e in v2}
print("removed:", sorted(k[1] for k in a - b))
print("added:", sorted((k[0], k[1]) for k in b - a))
EOF
```

期望：
- 生成器输出 `共 559 条 → …/dataset_v2.jsonl`，其中 `C_task 12`、`D_template 21`。
- `v1 unchanged`。
- `removed` 恰为 7 条：扇形扫掠 3 条、编队巡逻 2 条、会合集结 2 条。
- `added` 恰为 7 条：4 条 `C_task`，3 条 `D_template`。

- [ ] **Step 6: run_eval.py**

`build_context` 中的 `"templates"` 名单改为：

```python
        "templates": [
            "区域搜索后返航",
            "编队巡航后返航",
            "快速出动待命",
            "全员返航降落",
            "平行网格扫掠",
            "多机接力值守",
        ],
```

`main()` 中的 `ap.add_argument("--dataset", default=str(HERE / "dataset_v1.jsonl"))` 改为 `dataset_v2.jsonl`。

- [ ] **Step 7: validate_assets.py 改为比对 v2**

```bash
cd /media/hello/data1/Jinqiang/QGC/qgroundcontrol
sed -i 's/dataset_v1\.jsonl/dataset_v2.jsonl/g' swarm_ext/tests/validate_assets.py
grep -n 'dataset_v' swarm_ext/tests/validate_assets.py     # 期望 3 处，均为 dataset_v2.jsonl
```

- [ ] **Step 8: README**

`swarm_ext/tests/llm_eval/README.md`：
- 标题 `# LLM 集群指令评测集 v1（WP1.4）` 改为 `# LLM 集群指令评测集 v2（WP1.4）`。
- 「文件」表格中的 `dataset_v1.jsonl` 一行替换为两行：

```markdown
| `dataset_v2.jsonl` | 生成产物（当前版本）：一行一样本 `{id, category, instruction, online_ids, expected, note}` |
| `dataset_v1.jsonl` | 冻结的 v1 评测集；`BASELINE_kimi_k26.md` 与 `reports/` 下的报告均基于 v1 |
```

- 「样本类别」表格中 D 行的 `4 套内置模板 × 参数组合` 改为 `6 套内置模板 × 参数组合`。
- 在「## 样本类别」之前插入：

```markdown
## 版本

**v2（2026-10）**：内置模板由 9 套精简为 6 套（见
`swarm_ext/docs/specs/2026-10-09-task-template-simplification-design.md`），评测集随之调整，总数仍为 559 条：

- 「会合集结」两题改为期望「编队巡航后返航」，参数为 `formation: circle`（另一题加 `spacing: 25`、`cruise_seconds: 120`）；
- 「编队巡逻模板，默认执行」改为「编队巡航后返航模板，默认参数」；
- 「扇形扫掠」三题和「编队巡逻，楔形巡逻5分钟换纵队」去掉模板名，改为 C 类自定义编排（5 / 6 阶段）；
- 模拟上下文的模板名单改为 6 套。

题目移类后编号顺延，v2 与 v1 的同号样本不一定是同一道题。v1 的基线不能直接与 v2 比较，需在 v2 上重跑。
```

- [ ] **Step 9: 校验（与 CI 的 llm-eval-sanity 一致）并 lint**

```bash
cd /media/hello/data1/Jinqiang/QGC/qgroundcontrol/swarm_ext/tests/llm_eval
python3 -m py_compile *.py
python3 generate_dataset.py /tmp/d1.jsonl && python3 generate_dataset.py /tmp/d2.jsonl
diff /tmp/d1.jsonl /tmp/d2.jsonl && cmp /tmp/d1.jsonl dataset_v2.jsonl && test "$(wc -l < /tmp/d1.jsonl)" -ge 500 && echo "dataset OK"
python3 run_eval.py --dry-run
cd /media/hello/data1/Jinqiang/QGC/qgroundcontrol
python3 swarm_ext/tests/validate_assets.py | tail -9
RUFF=/tmp/rv_ruff/x/ruff-0.15.20.data/scripts/ruff
$RUFF format swarm_ext/tests/llm_eval/generate_dataset.py swarm_ext/tests/llm_eval/run_eval.py swarm_ext/tests/validate_assets.py
$RUFF check swarm_ext/tests/llm_eval/generate_dataset.py swarm_ext/tests/llm_eval/run_eval.py swarm_ext/tests/validate_assets.py
```

期望：
- `dataset OK`。
- `[run_eval] 数据集 559 条，一致性校验通过`。
- `validate_assets.py` 输出 `"templates": 6`、`"llm_samples": 559`、`"issues": []`。
- ruff 输出 `All checks passed!`。

如果 `ruff format` 改动了 `generate_dataset.py`，必须重新运行 `python3 swarm_ext/tests/llm_eval/generate_dataset.py`，并再跑一次本步的 `cmp`。格式化只改代码排版、不改生成结果，`cmp` 应仍然通过。

---

### Task 4: 文档

**Files:**（均在 `swarm_ext/` 下）
- Modify: `docs/用户使用手册.md:692-706`
- Modify: `README.md:13`、`docs/FEATURES.md:11`、`docs/QUALITY_UPDATE_USAGE.md:43`、`docs/功能分析与手册核验.md:25`
- Modify: `docs/QGC集群指挥控制二次开发方案.md:289` 之后、`docs/大模型集群任务接口扩展方案.md:151` 之后

以下历史文档**不改**：`P2校验闸门与协同规划进阶说明.md`、`整体软件开发计划.md`、`docs/reports/`、既往 specs/plans（设计 §3.5）。

- [ ] **Step 1: 用户手册 §15.4**

- 标题 `### 15.4 九套内置模板的实际含义` 改为 `### 15.4 六套内置模板的实际含义`。
- 表格删除「扇形扫掠搜索」「编队巡逻」「会合集结」三行，其余行不动。
- 在表格之后、`模板的相机要求目前主要是提示…` 段落之前，插入一段（前后各空一行）：

```markdown
2026-10 起内置模板由九套精简为六套，「扇形扫掠搜索」「编队巡逻」「会合集结」不再内置。升级后首次启动时，模板目录中的这三个文件（不论是否改过）会被改名退役，例如 `会合集结.json` 改为 `会合集结.json.retired.bak`；该备份名已存在时，改用带时间戳的 `会合集结.json.retired-<时间戳>.bak`。退役后面板不再列出它们。需要继续使用时，去掉 `.retired.bak`（或 `.retired-<时间戳>.bak`）后缀、恢复为 `.json` 文件并点“刷新”，它会作为用户模板出现。会合集结可以改用「编队巡航后返航」：队形选 circle，编队间距填集结半径，巡航时长填保持时长；注意该模板的编队间距上限为 100 米（原集结半径上限 200 米）。三角度扫掠和巡逻中换队形，请按 15.6 自建模板，或让 AI 自定义编排。
```

- [ ] **Step 2: 其余现行文档改计数**

```bash
cd /media/hello/data1/Jinqiang/QGC/qgroundcontrol/swarm_ext
sed -i '13s/九套模板资产不代表九种完整业务闭环均可直接使用/六套内置模板不代表六种完整业务闭环均可直接使用/' README.md
sed -i '11s/9 套模板资产/6 套内置模板/' docs/FEATURES.md
sed -i '43s/九套内置模板/六套内置模板/' docs/QUALITY_UPDATE_USAGE.md
sed -i '25s/九套模板/六套模板/' docs/功能分析与手册核验.md
git diff --stat README.md docs/FEATURES.md docs/QUALITY_UPDATE_USAGE.md docs/功能分析与手册核验.md   # 期望 4 个文件各 1 行改动
```

- [ ] **Step 3: 两份技术方案加注**

`docs/QGC集群指挥控制二次开发方案.md`：第 4 条「任务模板库（SwarmTaskLibrary，v1.3）」段落最后一行（`…沉淀组织级标准作业流程。`）之后，加一行（与该段同样缩进 3 个空格）：

```markdown
   > 2026-10 更新：内置模板经 P2 扩充后精简为 6 套（区域搜索后返航 / 平行网格扫掠 / 编队巡航后返航 / 快速出动待命 / 全员返航降落 / 多机接力值守），见 `specs/2026-10-09-task-template-simplification-design.md`。
```

`docs/大模型集群任务接口扩展方案.md`：「参数化」条目（以 `字符串数字自动转数值）；仓库 templates/ 目录附 4 套内置模板的参考副本。` 结尾）之后，空一行再加：

```markdown
> 2026-10 更新：内置模板经 P2 扩充后精简为 6 套（区域搜索后返航 / 平行网格扫掠 / 编队巡航后返航 / 快速出动待命 / 全员返航降落 / 多机接力值守），见 `specs/2026-10-09-task-template-simplification-design.md`。
```

- [ ] **Step 4: 核对**

```bash
cd /media/hello/data1/Jinqiang/QGC/qgroundcontrol/swarm_ext
grep -n '九套\|9 套' README.md docs/FEATURES.md docs/QUALITY_UPDATE_USAGE.md docs/功能分析与手册核验.md docs/用户使用手册.md   # 期望只剩用户手册新增段落里的「由九套精简为六套」
grep -c '2026-10 更新' docs/QGC集群指挥控制二次开发方案.md docs/大模型集群任务接口扩展方案.md   # 期望各 1
```

---

### Task 5: 全量验证与收尾

**Files:** 无新改动（只在验证失败时修复）

- [ ] **Step 1: 编译与 lint**

```bash
cd /media/hello/data1/Jinqiang/QGC/qgroundcontrol
/tmp/qgc_build.sh 2>&1 | tail -3
clang-format --dry-run -Werror src/Swarm/SwarmTaskLibrary.cc src/Swarm/SwarmTaskLibrary.h \
  test/Swarm/SwarmRegressionTest.cc test/Swarm/SwarmRegressionTest.h && echo "clang-format OK"
RUFF=/tmp/rv_ruff/x/ruff-0.15.20.data/scripts/ruff
$RUFF check swarm_ext/tests/llm_eval/generate_dataset.py swarm_ext/tests/llm_eval/run_eval.py swarm_ext/tests/validate_assets.py
$RUFF format --check swarm_ext/tests/llm_eval/generate_dataset.py swarm_ext/tests/llm_eval/run_eval.py swarm_ext/tests/validate_assets.py
grep -n 'Q_ASSERT' src/Swarm/SwarmTaskLibrary.cc || echo "no Q_ASSERT"
python3 swarm_ext/tests/validate_assets.py | tail -9
```

期望：编译成功；输出 `clang-format OK`；ruff 两项都通过；输出 `no Q_ASSERT`；`validate_assets.py` 的 `"issues": []`。

- [ ] **Step 2: Swarm/Task 相关测试**

```bash
/tmp/qgc_ctest.sh -R '"Swarm|Task"'
```

期望全部通过。

- [ ] **Step 3: 完整套件（与 `just test` 一致）**

约 10 分钟，可在后台运行：

```bash
/tmp/qgc_ctest.sh -L '"Unit|Integration"' -LE '"Flaky|Network"' 2>&1 | tail -15
```

期望 `100% tests passed`，总数与基线一致（2026-10-07 为 251/251；新测试是已有类中的一个 slot，不增加 ctest 条目）。

- [ ] **Step 4: 汇总并询问用户**

```bash
git status --short
git diff --stat
```

向用户汇报：改动文件清单、测试结果、以及 `整体软件开发计划.md` 中「≥9 套模板可用」退出标准与本次精简的冲突（设计 §4）。然后询问是否提交，以及如何拆分提交。建议拆成三个：
1. `refactor(Swarm): trim built-in task templates from 9 to 6`：代码、测试、`swarm_ext/templates/`、文档、设计与计划。
2. `test(llm_eval): add dataset v2 for the 6-template library`：评测集 v2 及相关脚本。
3. 是否推送到 `originJin/main` 由用户另行决定。只推 originJin，不推 origin。

未得到用户同意前不提交、不推送。
