#include "SwarmTaskLibrary.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <algorithm>
#include <cmath>

#include "QGCApplication.h"
#include "SwarmAuditLogger.h"
#include "SwarmManager.h"
#include "SwarmTaskController.h"

// ---------------------------------------------------------------------------
// 内置模板（首次启动落盘，之后与用户模板同等待遇、可编辑）
// ---------------------------------------------------------------------------
namespace {

const char kTplSearchReturn[] = R"TPL({
  "name": "区域搜索后返航",
  "description": "三阶段：全体起飞→矩形区域条带覆盖搜索→完成后自动返航降落",
  "version": 1,
  "builtin_revision": 2,
  "parameters": [
    {"key":"altitude","label":"作业高度(米)","type":"number","default":30,"min":10,"max":120},
    {"key":"length","label":"区域长度(米)","type":"number","default":200,"min":20,"max":100000},
    {"key":"width","label":"区域宽度(米)","type":"number","default":150,"min":20,"max":100000},
    {"key":"angle","label":"航线角度(度)","type":"number","default":0,"min":0,"max":359},
    {"key":"spacing","label":"行距(米)","type":"number","default":20,"min":5,"max":200},
    {"key":"climb_gate","label":"升空判定高度(米)","type":"number","default":25,"min":5,"max":115}
  ],
  "stages": [
    {"name":"起飞","actions":[{"type":"takeoff","altitude":"${altitude}"}],
     "complete_when":{"kind":"all_airborne","min_altitude":"${climb_gate}"}},
    {"name":"区域搜索","actions":[
        {"type":"plan_coverage","center":"map_center","length":"${length}","width":"${width}",
         "angle":"${angle}","spacing":"${spacing}","altitude":"${altitude}"},
        {"type":"start_mission"}],
     "complete_when":{"kind":"all_mission_finished"}},
    {"name":"返航","actions":[{"type":"rtl"}],
     "complete_when":{"kind":"all_disarmed"}}
  ]
})TPL";

const char kTplFormationCruise[] = R"TPL({
  "name": "编队巡航后返航",
  "description": "全体起飞→按指定队形编队巡航→计时结束停止编队并返航",
  "version": 1,
  "builtin_revision": 2,
  "parameters": [
    {"key":"altitude","label":"起飞高度(米)","type":"number","default":30,"min":10,"max":120},
    {"key":"climb_gate","label":"升空判定高度(米)","type":"number","default":25,"min":5,"max":115},
    {"key":"leader_id","label":"长机编号","type":"integer","default":1},
    {"key":"formation","label":"队形(wedge/column/line/circle)","type":"string","default":"wedge"},
    {"key":"spacing","label":"编队间距(米)","type":"number","default":15,"min":5,"max":100},
    {"key":"cruise_seconds","label":"巡航时长(秒)","type":"number","default":120,"min":10,"max":7200}
  ],
  "stages": [
    {"name":"起飞","actions":[{"type":"takeoff","altitude":"${altitude}"}],
     "complete_when":{"kind":"all_airborne","min_altitude":"${climb_gate}"}},
    {"name":"编队","actions":[{"type":"start_formation","leader_id":"${leader_id}",
        "formation":"${formation}","spacing":"${spacing}","vertical_step":3}],
     "complete_when":{"kind":"formation_running"}},
    {"name":"巡航","actions":[],
     "complete_when":{"kind":"duration","seconds":"${cruise_seconds}"}},
    {"name":"返航","actions":[{"type":"stop_formation"},{"type":"rtl"}],
     "complete_when":{"kind":"all_disarmed"}}
  ]
})TPL";

const char kTplQuickLaunch[] = R"TPL({
  "name": "快速出动待命",
  "description": "全体快速起飞至指定高度悬停，等待人工进一步指令",
  "version": 1,
  "builtin_revision": 2,
  "parameters": [
    {"key":"altitude","label":"起飞高度(米)","type":"number","default":20,"min":5,"max":120},
    {"key":"climb_gate","label":"升空判定高度(米)","type":"number","default":15,"min":3,"max":115}
  ],
  "stages": [
    {"name":"起飞","actions":[{"type":"takeoff","altitude":"${altitude}"}],
     "complete_when":{"kind":"all_airborne","min_altitude":"${climb_gate}"}},
    {"name":"待命","actions":[],
     "complete_when":{"kind":"manual"}}
  ]
})TPL";

const char kTplAllReturn[] = R"TPL({
  "name": "全员返航降落",
  "description": "停止编队并立即返航降落，适用于任务收尾或紧急回收",
  "version": 1,
  "builtin_revision": 2,
  "parameters": [],
  "stages": [
    {"name":"返航","actions":[{"type":"stop_formation"},{"type":"rtl"}],
     "complete_when":{"kind":"all_disarmed","timeout_sec":300}}
  ]
})TPL";

const char kTplGridSweep[] = R"TPL({
  "name": "平行网格扫掠",
  "description": "双程网格扫掠：起飞后先按角度一覆盖，再旋转至角度二二次覆盖，提高目标发现率，完成后返航",
  "version": 1,
  "builtin_revision": 2,
  "schema_version": 2,
  "tags": ["搜索", "扫掠", "网格"],
  "min_fleet": 2,
  "required_payload": "camera",
  "parameters": [
    {"key":"altitude","label":"作业高度（米）","type":"number","default":30,"min":10,"max":120},
    {"key":"climb_gate","label":"升空判定高度（米）","type":"number","default":25,"min":5,"max":115},
    {"key":"length","label":"区域长度（米）","type":"number","default":400,"min":20,"max":100000},
    {"key":"width","label":"区域宽度（米）","type":"number","default":300,"min":20,"max":100000},
    {"key":"spacing","label":"行距（米）","type":"number","default":20,"min":5,"max":200},
    {"key":"angle1","label":"第一程角度（度）","type":"number","default":0,"min":0,"max":359},
    {"key":"angle2","label":"第二程角度（度）","type":"number","default":90,"min":0,"max":359}
  ],
  "stages": [
    {"name":"起飞","actions":[{"type":"takeoff","altitude":"${altitude}"}],
     "complete_when":{"kind":"all_airborne","min_altitude":"${climb_gate}","timeout_sec":180}},
    {"name":"第一程扫掠","actions":[
        {"type":"plan_coverage","center":"map_center","length":"${length}","width":"${width}",
         "angle":"${angle1}","spacing":"${spacing}","altitude":"${altitude}"},
        {"type":"start_mission"}],
     "complete_when":{"kind":"all_mission_finished"}},
    {"name":"第二程扫掠","actions":[
        {"type":"plan_coverage","center":"map_center","length":"${length}","width":"${width}",
         "angle":"${angle2}","spacing":"${spacing}","altitude":"${altitude}"},
        {"type":"start_mission"}],
     "complete_when":{"kind":"all_mission_finished"}},
    {"name":"返航","actions":[{"type":"rtl"}],
     "complete_when":{"kind":"all_disarmed","timeout_sec":600}}
  ]
})TPL";

const char kTplRelayWatch[] = R"TPL({
  "name": "多机接力值守",
  "description": "分组接力空中值守：全体起飞后 A 组先值守，B 组到点接替，最后全体返航（P3 接入感知后升级为接力跟踪）",
  "version": 1,
  "builtin_revision": 2,
  "schema_version": 2,
  "tags": ["接力", "值守"],
  "min_fleet": 4,
  "required_payload": "camera",
  "parameters": [
    {"key":"altitude","label":"作业高度（米）","type":"number","default":30,"min":10,"max":120},
    {"key":"climb_gate","label":"升空判定高度（米）","type":"number","default":25,"min":5,"max":115},
    {"key":"group_a_ids","label":"A 组机号列表","type":"array","default":[]},
    {"key":"group_b_ids","label":"B 组机号列表","type":"array","default":[]},
    {"key":"watch_seconds","label":"每组值守时长（秒）","type":"integer","default":300,"min":30,"max":7200}
  ],
  "stages": [
    {"name":"起飞","actions":[{"type":"takeoff","altitude":"${altitude}"}],
     "complete_when":{"kind":"all_airborne","min_altitude":"${climb_gate}","timeout_sec":180}},
    {"name":"A组值守","actions":[{"type":"set_selection","vehicle_ids":"${group_a_ids}"}],
     "complete_when":{"kind":"duration","seconds":"${watch_seconds}"}},
    {"name":"B组接替","actions":[{"type":"set_selection","vehicle_ids":"${group_b_ids}"}],
     "complete_when":{"kind":"duration","seconds":"${watch_seconds}"}},
    {"name":"全体返航","actions":[{"type":"set_selection","vehicle_ids":"${all_vehicle_ids}"},{"type":"rtl"}],
     "complete_when":{"kind":"all_disarmed","timeout_sec":600}}
  ]
})TPL";

}  // namespace

SwarmTaskLibrary::SwarmTaskLibrary(SwarmManager* swarmManager)
    : QObject(swarmManager),
      _swarmManager(swarmManager),
      _templateDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                   QStringLiteral("/SwarmTemplates"))
{
    QDir().mkpath(_templateDir);
    (void) connect(this, &SwarmTaskLibrary::libraryFeedback, qgcApp(),
                   [](const QString& message) { qgcApp()->showAppMessage(message); });
    _retireRemovedBuiltins();
    _ensureDefaults();
    _scan();
}

SwarmTaskLibrary::~SwarmTaskLibrary() = default;

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
        const QString target = _templateDir + QLatin1Char('/') + backupName;
        QFile retired(path);
        if (!retired.rename(target)) {
            retiredAll = false;
            emit libraryFeedback(tr("旧模板退役失败：%1（%2）").arg(path, retired.errorString()));
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

void SwarmTaskLibrary::_ensureDefaults()
{
    static const struct
    {
        const char* fileName;
        const char* json;
    } defaults[] = {
        {"区域搜索后返航.json", kTplSearchReturn}, {"编队巡航后返航.json", kTplFormationCruise},
        {"快速出动待命.json", kTplQuickLaunch},    {"全员返航降落.json", kTplAllReturn},
        {"平行网格扫掠.json", kTplGridSweep},      {"多机接力值守.json", kTplRelayWatch},
    };

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
    for (const auto& def : defaults) {
        const QString path = _templateDir + QLatin1Char('/') + QString::fromUtf8(def.fileName);
        QFile previous(path);
        if (previous.exists()) {
            if (!previous.open(QIODevice::ReadOnly)) {
                emit libraryFeedback(tr("模板无法读取：%1").arg(path));
                continue;
            }
            const QByteArray oldBytes = previous.readAll();
            previous.close();
            const auto oldObject = QJsonDocument::fromJson(oldBytes).object();
            const QByteArray digest = QCryptographicHash::hash(QJsonDocument(oldObject).toJson(QJsonDocument::Compact),
                                                               QCryptographicHash::Sha256)
                                          .toHex();
            if (digest != legacyDigests.value(oldObject.value(QStringLiteral("name")).toString()))
                continue;
            QSaveFile backup(path + QStringLiteral(".pre-revision-2.bak"));
            if (!backup.open(QIODevice::WriteOnly) || backup.write(oldBytes) != oldBytes.size() || !backup.commit()) {
                emit libraryFeedback(tr("模板备份失败：%1").arg(path));
                continue;
            }
        }
        QSaveFile file(path);
        const QByteArray bytes(def.json);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            emit libraryFeedback(tr("模板更新失败：%1").arg(path));
    }
}

void SwarmTaskLibrary::_scan()
{
    static const QSet<QString> builtInNames = {QStringLiteral("区域搜索后返航"), QStringLiteral("编队巡航后返航"),
                                               QStringLiteral("快速出动待命"),   QStringLiteral("全员返航降落"),
                                               QStringLiteral("平行网格扫掠"),   QStringLiteral("多机接力值守")};

    _templates.clear();
    _templateList.clear();

    const QStringList files = QDir(_templateDir).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& fileName : files) {
        QFile file(_templateDir + QLatin1Char('/') + fileName);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (!doc.isObject()) {
            emit libraryFeedback(tr("模板 %1 解析失败（非 JSON 对象），已跳过").arg(fileName));
            continue;
        }
        const QJsonObject obj = doc.object();
        const QString name = obj.value(QStringLiteral("name")).toString();
        const QJsonArray stages = obj.value(QStringLiteral("stages")).toArray();
        if (name.isEmpty() || stages.isEmpty()) {
            emit libraryFeedback(tr("模板 %1 缺少 name 或 stages，已跳过").arg(fileName));
            continue;
        }

        TaskTemplate tpl;
        tpl.name = name;
        tpl.description = obj.value(QStringLiteral("description")).toString();
        tpl.filePath = file.fileName();
        tpl.builtIn = builtInNames.contains(name);
        tpl.parameters = obj.value(QStringLiteral("parameters")).toArray();
        tpl.stages = stages;
        // Schema v2 扩展字段（缺省兼容 v1）
        tpl.schemaVersion = obj.value(QStringLiteral("schema_version")).toInt(1);
        tpl.minFleet = obj.value(QStringLiteral("min_fleet")).toInt(0);
        tpl.requiredPayload = obj.value(QStringLiteral("required_payload")).toString();
        for (const QJsonValue& tv : obj.value(QStringLiteral("tags")).toArray()) {
            tpl.tags.append(tv.toString());
        }
        _templates.append(tpl);

        QVariantMap summary;
        summary[QStringLiteral("name")] = tpl.name;
        summary[QStringLiteral("description")] = tpl.description;
        summary[QStringLiteral("stageCount")] = tpl.stages.count();
        summary[QStringLiteral("builtIn")] = tpl.builtIn;
        summary[QStringLiteral("parameters")] = tpl.parameters.toVariantList();
        summary[QStringLiteral("tags")] = tpl.tags;
        summary[QStringLiteral("minFleet")] = tpl.minFleet;
        _templateList.append(summary);
    }
    emit templatesChanged();
}

const SwarmTaskLibrary::TaskTemplate* SwarmTaskLibrary::_find(const QString& name) const
{
    for (const TaskTemplate& tpl : _templates) {
        if (tpl.name == name) {
            return &tpl;
        }
    }
    return nullptr;
}

// 递归替换：字符串值恰好为 "${key}" 时替换为参数值（保留类型）
QJsonValue SwarmTaskLibrary::_substitute(const QJsonValue& value, const QJsonObject& params)
{
    if (value.isObject()) {
        QJsonObject out;
        const QJsonObject obj = value.toObject();
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            out.insert(it.key(), _substitute(it.value(), params));
        }
        return out;
    }
    if (value.isArray()) {
        QJsonArray out;
        for (const QJsonValue& v : value.toArray()) {
            out.append(_substitute(v, params));
        }
        return out;
    }
    if (value.isString()) {
        const QString str = value.toString();
        if (str.startsWith(QStringLiteral("${")) && str.endsWith(QLatin1Char('}'))) {
            const QString key = str.mid(2, str.length() - 3);
            if (params.contains(key)) {
                return params.value(key);
            }
        }
        return value;
    }
    return value;
}

QJsonArray SwarmTaskLibrary::instantiate(const QString& name, const QVariantMap& paramValues, QString& error) const
{
    const TaskTemplate* tpl = _find(name);
    if (!tpl) {
        error = tr("模板「%1」不存在").arg(name);
        return QJsonArray();
    }

    error.clear();
    const QList<int> targets = _swarmManager->boundTargetIds();
    if (targets.isEmpty() || targets.size() < tpl->minFleet) {
        error = tr("绑定目标数量不足：至少需要 %1 架").arg(qMax(1, tpl->minFleet));
        return {};
    }
    QJsonObject effectiveParams;
    QSet<QString> declared;
    for (const auto& parameter : tpl->parameters)
        declared.insert(parameter.toObject().value(QStringLiteral("key")).toString());
    for (auto it = paramValues.begin(); it != paramValues.end(); ++it) {
        if (!declared.contains(it.key())) {
            error = tr("未知参数：%1").arg(it.key());
            return {};
        }
    }
    for (const QJsonValue& pv : tpl->parameters) {
        const QJsonObject p = pv.toObject();
        const QString key = p.value(QStringLiteral("key")).toString();
        const QString type = p.value(QStringLiteral("type")).toString();
        QJsonValue value = paramValues.contains(key) ? QJsonValue::fromVariant(paramValues.value(key))
                                                     : p.value(QStringLiteral("default"));
        if (!paramValues.contains(key)) {
            if (key == QStringLiteral("leader_id"))
                value = targets.first();
            if (key == QStringLiteral("group_a_ids") || key == QStringLiteral("group_b_ids")) {
                QJsonArray group;
                const int split = (targets.size() + 1) / 2;
                for (int i = 0; i < targets.size(); ++i)
                    if ((key == QStringLiteral("group_a_ids")) == (i < split))
                        group.append(targets[i]);
                value = group;
            }
            if (key == QStringLiteral("climb_gate") && effectiveParams.contains(QStringLiteral("altitude")))
                value = qBound(p.value(QStringLiteral("min")).toDouble(),
                               effectiveParams.value(QStringLiteral("altitude")).toDouble() - 5.0,
                               p.value(QStringLiteral("max")).toDouble());
        }
        if (type == QStringLiteral("number") || type == QStringLiteral("integer")) {
            if (value.isString()) {
                static const QRegularExpression number(
                    QStringLiteral(R"(^[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?$)"));
                const QString text = value.toString().trimmed();
                bool ok = false;
                const double converted = text.toDouble(&ok);
                if (!number.match(text).hasMatch() || !ok) {
                    error = tr("%1：必须输入完整有效数字").arg(key);
                    return {};
                }
                value = converted;
            }
            const double n = value.toDouble();
            if (!value.isDouble() || !std::isfinite(n) || (type == QStringLiteral("integer") && std::trunc(n) != n) ||
                (p.contains(QStringLiteral("min")) && n < p.value(QStringLiteral("min")).toDouble()) ||
                (p.contains(QStringLiteral("max")) && n > p.value(QStringLiteral("max")).toDouble())) {
                error = tr("%1：数值类型或范围无效").arg(key);
                return {};
            }
        } else if (type == QStringLiteral("array")) {
            if (value.isString()) {
                QJsonParseError parseError;
                const auto parsed = QJsonDocument::fromJson(value.toString().toUtf8(), &parseError);
                if (parseError.error != QJsonParseError::NoError || !parsed.isArray()) {
                    error = tr("%1：必须为完整 JSON 数组").arg(key);
                    return {};
                }
                value = parsed.array();
            }
            if (!value.isArray() || value.toArray().isEmpty()) {
                error = tr("%1：数组不能为空").arg(key);
                return {};
            }
            QSet<int> seen;
            for (const auto& entry : value.toArray()) {
                const int id = entry.toInt(-1);
                if (!entry.isDouble() || entry.toDouble() != id || !targets.contains(id) || seen.contains(id)) {
                    error = tr("%1：机号重复或不在绑定目标中").arg(key);
                    return {};
                }
                seen.insert(id);
            }
        } else if (type != QStringLiteral("string") || !value.isString()) {
            error = tr("%1：参数类型无效").arg(key);
            return {};
        }
        effectiveParams.insert(key, value);
    }
    if (effectiveParams.contains(QStringLiteral("climb_gate")) &&
        effectiveParams.value(QStringLiteral("climb_gate")).toDouble() >
            effectiveParams.value(QStringLiteral("altitude")).toDouble()) {
        error = tr("升空判定高度不能高于起飞高度");
        return {};
    }
    QJsonArray allIds;
    for (int id : targets)
        allIds.append(id);
    effectiveParams[QStringLiteral("all_vehicle_ids")] = allIds;
    if (effectiveParams.contains(QStringLiteral("group_a_ids"))) {
        QSet<int> unionIds;
        for (const auto& key : {QStringLiteral("group_a_ids"), QStringLiteral("group_b_ids")}) {
            for (const auto& v : effectiveParams.value(key).toArray()) {
                if (unionIds.contains(v.toInt())) {
                    error = tr("接力分组不得重叠");
                    return {};
                }
                unionIds.insert(v.toInt());
            }
        }
        if (unionIds != QSet<int>(targets.begin(), targets.end())) {
            error = tr("接力分组必须覆盖全部绑定目标");
            return {};
        }
    }
    QJsonArray stages = _substitute(tpl->stages, effectiveParams).toArray();
    for (int i = 0; i < stages.size(); ++i) {
        auto stage = stages[i].toObject();
        auto condition = stage.value(QStringLiteral("complete_when")).toObject();
        if (condition.value(QStringLiteral("kind")) == QStringLiteral("duration") &&
            !condition.contains(QStringLiteral("timeout_sec"))) {
            condition[QStringLiteral("timeout_sec")] =
                qMax(600, condition.value(QStringLiteral("seconds")).toInt() +
                              condition.value(QStringLiteral("grace_sec")).toInt(3) + 30);
            stage[QStringLiteral("complete_when")] = condition;
            stages[i] = stage;
        }
    }
    QStringList errors;
    const auto parsed = _swarmManager->taskController()->parseStages(stages, QGeoCoordinate(0, 0), errors);
    if (parsed.isEmpty() || !errors.isEmpty()) {
        error = errors.join(QStringLiteral("；"));
        return {};
    }
    return stages;
}

QJsonArray SwarmTaskLibrary::contextSummary() const
{
    QJsonArray out;
    for (const TaskTemplate& tpl : _templates) {
        QJsonArray params;
        for (const QJsonValue& pv : tpl.parameters) {
            const QJsonObject p = pv.toObject();
            params.append(QJsonObject{{QStringLiteral("key"), p.value(QStringLiteral("key")).toString()},
                                      {QStringLiteral("label"), p.value(QStringLiteral("label")).toString()},
                                      {QStringLiteral("default"), p.value(QStringLiteral("default"))}});
        }
        out.append(QJsonObject{{QStringLiteral("name"), tpl.name},
                               {QStringLiteral("description"), tpl.description},
                               {QStringLiteral("stage_count"), tpl.stages.count()},
                               {QStringLiteral("tags"), QJsonArray::fromStringList(tpl.tags)},
                               {QStringLiteral("min_fleet"), tpl.minFleet},
                               {QStringLiteral("parameters"), params}});
    }
    return out;
}

QVariantMap SwarmTaskLibrary::templateMeta(const QString& name) const
{
    const TaskTemplate* tpl = _find(name);
    if (!tpl) {
        return {};
    }
    return {
        {QStringLiteral("tags"), tpl->tags},
        {QStringLiteral("min_fleet"), tpl->minFleet},
        {QStringLiteral("required_payload"), tpl->requiredPayload},
        {QStringLiteral("stage_count"), tpl->stages.count()},
        {QStringLiteral("schema_version"), tpl->schemaVersion},
    };
}

QVariantList SwarmTaskLibrary::templateParameters(const QString& name) const
{
    const TaskTemplate* tpl = _find(name);
    return tpl ? tpl->parameters.toVariantList() : QVariantList();
}

bool SwarmTaskLibrary::runTemplate(const QString& name, const QVariantMap& paramValues, double mapCenterLat,
                                   double mapCenterLon)
{
    SwarmAuditLogger::instance()->logTemplateRun(name, paramValues);

    // Schema v2 模板级校验（WP2.1）：规模与载荷
    if (const TaskTemplate* tpl = _find(name)) {
        const int online = _swarmManager->boundTargetIds().size();
        if (tpl->minFleet > 0 && online < tpl->minFleet) {
            emit libraryFeedback(
                tr("模板执行失败：「%1」要求最少 %2 架在线，当前仅 %3 架").arg(name).arg(tpl->minFleet).arg(online));
            return false;
        }
        if (!tpl->requiredPayload.isEmpty()) {
            emit libraryFeedback(tr("提示：模板要求载荷「%1」，请确认各机载荷配置").arg(tpl->requiredPayload));
        }
    }

    QString error;
    const QJsonArray stagesJson = instantiate(name, paramValues, error);
    if (stagesJson.isEmpty()) {
        emit libraryFeedback(tr("模板执行失败：%1").arg(error));
        return false;
    }

    QStringList warnings;
    const QList<SwarmTaskController::Stage> stages =
        _swarmManager->taskController()->parseStages(stagesJson, QGeoCoordinate(mapCenterLat, mapCenterLon), warnings);
    for (const QString& w : warnings) {
        emit libraryFeedback(w);
    }
    if (stages.isEmpty()) {
        emit libraryFeedback(tr("模板执行失败：没有有效阶段"));
        return false;
    }
    return _swarmManager->taskController()->startTask(name, stages, true);
}

bool SwarmTaskLibrary::saveTemplate(const QString& name, const QString& description, const QJsonArray& stages,
                                    const QJsonArray& parameters)
{
    if (name.trimmed().isEmpty() || stages.isEmpty()) {
        emit libraryFeedback(tr("保存失败：模板名或阶段为空"));
        return false;
    }
    QJsonObject obj;
    obj[QStringLiteral("name")] = name;
    obj[QStringLiteral("description")] = description;
    obj[QStringLiteral("version")] = 1;
    obj[QStringLiteral("parameters")] = parameters;
    obj[QStringLiteral("stages")] = stages;

    QString fileName = name;
    fileName.replace(QLatin1Char('/'), QLatin1Char('_')).replace(QLatin1Char('\\'), QLatin1Char('_'));
    QSaveFile file(_templateDir + QLatin1Char('/') + fileName + QStringLiteral(".json"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit libraryFeedback(tr("保存失败：无法写入 %1").arg(file.fileName()));
        return false;
    }
    const auto bytes = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        emit libraryFeedback(tr("保存模板失败"));
        return false;
    }
    _scan();
    emit libraryFeedback(tr("模板「%1」已保存").arg(name));
    return true;
}

bool SwarmTaskLibrary::deleteTemplate(const QString& name)
{
    const TaskTemplate* tpl = _find(name);
    if (!tpl) {
        return false;
    }
    if (tpl->builtIn) {
        emit libraryFeedback(tr("内置模板不可删除"));
        return false;
    }
    if (!QFile::remove(tpl->filePath)) {
        emit libraryFeedback(tr("删除失败：%1").arg(tpl->filePath));
        return false;
    }
    _scan();
    emit libraryFeedback(tr("模板「%1」已删除").arg(name));
    return true;
}

void SwarmTaskLibrary::reload()
{
    _scan();
    emit libraryFeedback(tr("模板库已刷新（%1 个模板）").arg(_templates.count()));
}

void SwarmTaskLibrary::openTemplateDir()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(_templateDir));
}
