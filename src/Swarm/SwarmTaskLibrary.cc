#include "SwarmTaskLibrary.h"

#include "SwarmManager.h"
#include "SwarmAuditLogger.h"
#include "SwarmTaskController.h"
#include "QGCApplication.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QStandardPaths>
#include <QtGui/QDesktopServices>
#include <QtCore/QUrl>

// ---------------------------------------------------------------------------
// 内置模板（首次启动落盘，之后与用户模板同等待遇、可编辑）
// ---------------------------------------------------------------------------
namespace {

const char kTplSearchReturn[] = R"TPL({
  "name": "区域搜索后返航",
  "description": "三阶段：全体起飞→矩形区域条带覆盖搜索→完成后自动返航降落",
  "version": 1,
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
  "parameters": [],
  "stages": [
    {"name":"返航","actions":[{"type":"stop_formation"},{"type":"rtl"}],
     "complete_when":{"kind":"all_disarmed","timeout_sec":300}}
  ]
})TPL";

const char kTplSectorSweep[] = R"TPL({
  "name": "扇形扫掠搜索",
  "description": "三扇区依次扫掠：全体起飞后按设定角度对三个扇区依次覆盖搜索，完成后返航",
  "version": 1,
  "schema_version": 2,
  "tags": ["搜索", "扫掠", "覆盖"],
  "min_fleet": 3,
  "required_payload": "camera",
  "parameters": [
    {"key":"altitude","label":"作业高度（米）","type":"number","default":30,"min":10,"max":120},
    {"key":"climb_gate","label":"升空判定高度（米）","type":"number","default":25,"min":5,"max":115},
    {"key":"length","label":"扇区长度（米）","type":"number","default":300,"min":20,"max":100000},
    {"key":"width","label":"扇区宽度（米）","type":"number","default":150,"min":20,"max":100000},
    {"key":"spacing","label":"行距（米）","type":"number","default":20,"min":5,"max":200},
    {"key":"angle1","label":"扇区1角度（度）","type":"number","default":0,"min":0,"max":359},
    {"key":"angle2","label":"扇区2角度（度）","type":"number","default":60,"min":0,"max":359},
    {"key":"angle3","label":"扇区3角度（度）","type":"number","default":120,"min":0,"max":359}
  ],
  "stages": [
    {"name":"起飞","actions":[{"type":"takeoff","altitude":"${altitude}"}],
     "complete_when":{"kind":"all_airborne","min_altitude":"${climb_gate}","timeout_sec":180}},
    {"name":"扇区1扫掠","actions":[
        {"type":"plan_coverage","center":"map_center","length":"${length}","width":"${width}",
         "angle":"${angle1}","spacing":"${spacing}","altitude":"${altitude}"},
        {"type":"start_mission"}],
     "complete_when":{"kind":"all_mission_finished"}},
    {"name":"扇区2扫掠","actions":[
        {"type":"plan_coverage","center":"map_center","length":"${length}","width":"${width}",
         "angle":"${angle2}","spacing":"${spacing}","altitude":"${altitude}"},
        {"type":"start_mission"}],
     "complete_when":{"kind":"all_mission_finished"}},
    {"name":"扇区3扫掠","actions":[
        {"type":"plan_coverage","center":"map_center","length":"${length}","width":"${width}",
         "angle":"${angle3}","spacing":"${spacing}","altitude":"${altitude}"},
        {"type":"start_mission"}],
     "complete_when":{"kind":"all_mission_finished"}},
    {"name":"返航","actions":[{"type":"rtl"}],
     "complete_when":{"kind":"all_disarmed","timeout_sec":600}}
  ]
})TPL";

const char kTplGridSweep[] = R"TPL({
  "name": "平行网格扫掠",
  "description": "双程网格扫掠：起飞后先按角度一覆盖，再旋转至角度二二次覆盖，提高目标发现率，完成后返航",
  "version": 1,
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

const char kTplFormationPatrol[] = R"TPL({
  "name": "编队巡逻",
  "description": "起飞后以主队形巡逻一段时间，变换队形继续巡逻，结束后停止编队返航",
  "version": 1,
  "schema_version": 2,
  "tags": ["巡逻", "编队"],
  "min_fleet": 3,
  "parameters": [
    {"key":"altitude","label":"作业高度（米）","type":"number","default":40,"min":10,"max":120},
    {"key":"climb_gate","label":"升空判定高度（米）","type":"number","default":35,"min":5,"max":115},
    {"key":"leader_id","label":"长机编号","type":"integer","default":1,"min":1,"max":250},
    {"key":"formation","label":"主队形（wedge/column/line/circle）","type":"string","default":"wedge"},
    {"key":"spacing","label":"编队间距（米）","type":"number","default":15,"min":2,"max":500},
    {"key":"patrol_seconds","label":"第一程巡逻时长（秒）","type":"integer","default":180,"min":10,"max":7200},
    {"key":"second_formation","label":"第二队形（wedge/column/line/circle）","type":"string","default":"column"},
    {"key":"patrol_seconds_2","label":"第二程巡逻时长（秒）","type":"integer","default":180,"min":10,"max":7200}
  ],
  "stages": [
    {"name":"起飞","actions":[{"type":"takeoff","altitude":"${altitude}"}],
     "complete_when":{"kind":"all_airborne","min_altitude":"${climb_gate}","timeout_sec":180}},
    {"name":"主队形就位","actions":[
        {"type":"start_formation","leader_id":"${leader_id}","formation":"${formation}","spacing":"${spacing}"}],
     "complete_when":{"kind":"formation_running"}},
    {"name":"第一程巡逻","actions":[],
     "complete_when":{"kind":"duration","seconds":"${patrol_seconds}"}},
    {"name":"变换队形","actions":[
        {"type":"start_formation","leader_id":"${leader_id}","formation":"${second_formation}","spacing":"${spacing}"}],
     "complete_when":{"kind":"formation_running"}},
    {"name":"第二程巡逻","actions":[],
     "complete_when":{"kind":"duration","seconds":"${patrol_seconds_2}"}},
    {"name":"返航","actions":[{"type":"stop_formation"},{"type":"rtl"}],
     "complete_when":{"kind":"all_disarmed","timeout_sec":600}}
  ]
})TPL";

const char kTplRelayWatch[] = R"TPL({
  "name": "多机接力值守",
  "description": "分组接力空中值守：全体起飞后 A 组先值守，B 组到点接替，最后全体返航（P3 接入感知后升级为接力跟踪）",
  "version": 1,
  "schema_version": 2,
  "tags": ["接力", "值守"],
  "min_fleet": 4,
  "required_payload": "camera",
  "parameters": [
    {"key":"altitude","label":"作业高度（米）","type":"number","default":30,"min":10,"max":120},
    {"key":"climb_gate","label":"升空判定高度（米）","type":"number","default":25,"min":5,"max":115},
    {"key":"group_a_ids","label":"A 组机号列表","type":"array","default":[1,2,3,4]},
    {"key":"group_b_ids","label":"B 组机号列表","type":"array","default":[5,6,7,8]},
    {"key":"watch_seconds","label":"每组值守时长（秒）","type":"integer","default":300,"min":30,"max":7200}
  ],
  "stages": [
    {"name":"起飞","actions":[{"type":"takeoff","altitude":"${altitude}"}],
     "complete_when":{"kind":"all_airborne","min_altitude":"${climb_gate}","timeout_sec":180}},
    {"name":"A组值守","actions":[{"type":"set_selection","vehicle_ids":"${group_a_ids}"}],
     "complete_when":{"kind":"duration","seconds":"${watch_seconds}"}},
    {"name":"B组接替","actions":[{"type":"set_selection","vehicle_ids":"${group_b_ids}"}],
     "complete_when":{"kind":"duration","seconds":"${watch_seconds}"}},
    {"name":"全体返航","actions":[{"type":"set_selection","vehicle_ids":[]},{"type":"rtl"}],
     "complete_when":{"kind":"all_disarmed","timeout_sec":600}}
  ]
})TPL";

const char kTplRendezvous[] = R"TPL({
  "name": "会合集结",
  "description": "起飞后以长机为集结点组成环形编队收拢，保持后全体返航",
  "version": 1,
  "schema_version": 2,
  "tags": ["集结", "编队"],
  "min_fleet": 3,
  "parameters": [
    {"key":"altitude","label":"集结高度（米）","type":"number","default":30,"min":10,"max":120},
    {"key":"climb_gate","label":"升空判定高度（米）","type":"number","default":25,"min":5,"max":115},
    {"key":"leader_id","label":"集结长机编号","type":"integer","default":1,"min":1,"max":250},
    {"key":"gather_radius","label":"集结半径（米）","type":"number","default":20,"min":5,"max":200},
    {"key":"hold_seconds","label":"集结保持时长（秒）","type":"integer","default":60,"min":10,"max":3600}
  ],
  "stages": [
    {"name":"起飞","actions":[{"type":"takeoff","altitude":"${altitude}"}],
     "complete_when":{"kind":"all_airborne","min_altitude":"${climb_gate}","timeout_sec":180}},
    {"name":"集结","actions":[
        {"type":"start_formation","leader_id":"${leader_id}","formation":"circle","spacing":"${gather_radius}"}],
     "complete_when":{"kind":"formation_running"}},
    {"name":"集结保持","actions":[],
     "complete_when":{"kind":"duration","seconds":"${hold_seconds}"}},
    {"name":"返航","actions":[{"type":"stop_formation"},{"type":"rtl"}],
     "complete_when":{"kind":"all_disarmed","timeout_sec":600}}
  ]
})TPL";

} // namespace

SwarmTaskLibrary::SwarmTaskLibrary(SwarmManager *swarmManager)
    : QObject(swarmManager)
    , _swarmManager(swarmManager)
    , _templateDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                   + QStringLiteral("/SwarmTemplates"))
{
    QDir().mkpath(_templateDir);
    _ensureDefaults();
    _scan();
    (void) connect(this, &SwarmTaskLibrary::libraryFeedback, qgcApp(), [](const QString &message) {
        qgcApp()->showAppMessage(message);
    });
}

SwarmTaskLibrary::~SwarmTaskLibrary() = default;

void SwarmTaskLibrary::_ensureDefaults()
{
    static const struct { const char *fileName; const char *json; } defaults[] = {
        {"区域搜索后返航.json",   kTplSearchReturn},
        {"编队巡航后返航.json",   kTplFormationCruise},
        {"快速出动待命.json",     kTplQuickLaunch},
        {"全员返航降落.json",     kTplAllReturn},
        {"扇形扫掠搜索.json",     kTplSectorSweep},
        {"平行网格扫掠.json",     kTplGridSweep},
        {"编队巡逻.json",         kTplFormationPatrol},
        {"多机接力值守.json",     kTplRelayWatch},
        {"会合集结.json",         kTplRendezvous},
    };
    for (const auto &def : defaults) {
        const QString path = _templateDir + QLatin1Char('/') + QString::fromUtf8(def.fileName);
        if (!QFile::exists(path)) {
            QFile file(path);
            if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                file.write(def.json);
            }
        }
    }
}

void SwarmTaskLibrary::_scan()
{
    static const QSet<QString> builtInNames = {
        QStringLiteral("区域搜索后返航"), QStringLiteral("编队巡航后返航"),
        QStringLiteral("快速出动待命"), QStringLiteral("全员返航降落"),
        QStringLiteral("扇形扫掠搜索"), QStringLiteral("平行网格扫掠"),
        QStringLiteral("编队巡逻"),     QStringLiteral("多机接力值守"),
        QStringLiteral("会合集结")
    };

    _templates.clear();
    _templateList.clear();

    const QStringList files = QDir(_templateDir).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString &fileName : files) {
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
        tpl.name        = name;
        tpl.description = obj.value(QStringLiteral("description")).toString();
        tpl.filePath    = file.fileName();
        tpl.builtIn     = builtInNames.contains(name);
        tpl.parameters  = obj.value(QStringLiteral("parameters")).toArray();
        tpl.stages      = stages;
        // Schema v2 扩展字段（缺省兼容 v1）
        tpl.schemaVersion   = obj.value(QStringLiteral("schema_version")).toInt(1);
        tpl.minFleet        = obj.value(QStringLiteral("min_fleet")).toInt(0);
        tpl.requiredPayload = obj.value(QStringLiteral("required_payload")).toString();
        for (const QJsonValue &tv : obj.value(QStringLiteral("tags")).toArray()) {
            tpl.tags.append(tv.toString());
        }
        _templates.append(tpl);

        QVariantMap summary;
        summary[QStringLiteral("name")]        = tpl.name;
        summary[QStringLiteral("description")] = tpl.description;
        summary[QStringLiteral("stageCount")]  = tpl.stages.count();
        summary[QStringLiteral("builtIn")]     = tpl.builtIn;
        summary[QStringLiteral("parameters")]  = tpl.parameters.toVariantList();
        summary[QStringLiteral("tags")]        = tpl.tags;
        summary[QStringLiteral("minFleet")]    = tpl.minFleet;
        _templateList.append(summary);
    }
    emit templatesChanged();
}

const SwarmTaskLibrary::TaskTemplate *SwarmTaskLibrary::_find(const QString &name) const
{
    for (const TaskTemplate &tpl : _templates) {
        if (tpl.name == name) {
            return &tpl;
        }
    }
    return nullptr;
}

// 递归替换：字符串值恰好为 "${key}" 时替换为参数值（保留类型）
QJsonValue SwarmTaskLibrary::_substitute(const QJsonValue &value, const QJsonObject &params)
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
        for (const QJsonValue &v : value.toArray()) {
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

QJsonArray SwarmTaskLibrary::instantiate(const QString &name, const QVariantMap &paramValues, QString &error) const
{
    const TaskTemplate *tpl = _find(name);
    if (!tpl) {
        error = tr("模板「%1」不存在").arg(name);
        return QJsonArray();
    }

    // 有效参数 = 模板默认值 ← 调用方覆盖，数值按 min/max 限幅
    QJsonObject effectiveParams;
    for (const QJsonValue &pv : tpl->parameters) {
        const QJsonObject p = pv.toObject();
        const QString key = p.value(QStringLiteral("key")).toString();
        QJsonValue value = p.value(QStringLiteral("default"));
        if (paramValues.contains(key) && paramValues.value(key).isValid()) {
            value = QJsonValue::fromVariant(paramValues.value(key));
        }
        // 类型矫正：声明为数值的参数收到字符串时转换（AI/QML 均可能传入字符串）
        const QString ptype = p.value(QStringLiteral("type")).toString();
        if ((ptype == QStringLiteral("number") || ptype == QStringLiteral("integer")) && value.isString()) {
            value = value.toString().toDouble();
        }
        if (value.isDouble()) {
            double v = value.toDouble();
            if (p.contains(QStringLiteral("min"))) {
                v = qMax(v, p.value(QStringLiteral("min")).toDouble());
            }
            if (p.contains(QStringLiteral("max"))) {
                v = qMin(v, p.value(QStringLiteral("max")).toDouble());
            }
            value = v;
        }
        effectiveParams.insert(key, value);
    }

    error.clear();
    return _substitute(tpl->stages, effectiveParams).toArray();
}

QJsonArray SwarmTaskLibrary::contextSummary() const
{
    QJsonArray out;
    for (const TaskTemplate &tpl : _templates) {
        QJsonArray params;
        for (const QJsonValue &pv : tpl.parameters) {
            const QJsonObject p = pv.toObject();
            params.append(QJsonObject{
                {QStringLiteral("key"), p.value(QStringLiteral("key")).toString()},
                {QStringLiteral("label"), p.value(QStringLiteral("label")).toString()},
                {QStringLiteral("default"), p.value(QStringLiteral("default"))}
            });
        }
        out.append(QJsonObject{
            {QStringLiteral("name"), tpl.name},
            {QStringLiteral("description"), tpl.description},
            {QStringLiteral("stage_count"), tpl.stages.count()},
            {QStringLiteral("tags"), QJsonArray::fromStringList(tpl.tags)},
            {QStringLiteral("min_fleet"), tpl.minFleet},
            {QStringLiteral("parameters"), params}
        });
    }
    return out;
}

QVariantMap SwarmTaskLibrary::templateMeta(const QString &name) const
{
    const TaskTemplate *tpl = _find(name);
    if (!tpl) {
        return {};
    }
    return {
        {QStringLiteral("tags"),             tpl->tags},
        {QStringLiteral("min_fleet"),        tpl->minFleet},
        {QStringLiteral("required_payload"), tpl->requiredPayload},
        {QStringLiteral("stage_count"),      tpl->stages.count()},
        {QStringLiteral("schema_version"),   tpl->schemaVersion},
    };
}

QVariantList SwarmTaskLibrary::templateParameters(const QString &name) const
{
    const TaskTemplate *tpl = _find(name);
    return tpl ? tpl->parameters.toVariantList() : QVariantList();
}

bool SwarmTaskLibrary::runTemplate(const QString &name, const QVariantMap &paramValues,
                                   double mapCenterLat, double mapCenterLon)
{
    SwarmAuditLogger::instance()->logTemplateRun(name, paramValues);

    // Schema v2 模板级校验（WP2.1）：规模与载荷
    if (const TaskTemplate *tpl = _find(name)) {
        const int online = _swarmManager->vehicleCount();
        if (tpl->minFleet > 0 && online < tpl->minFleet) {
            emit libraryFeedback(tr("模板执行失败：「%1」要求最少 %2 架在线，当前仅 %3 架")
                                     .arg(name).arg(tpl->minFleet).arg(online));
            return false;
        }
        if (!tpl->requiredPayload.isEmpty()) {
            emit libraryFeedback(tr("提示：模板要求载荷「%1」，请确认各机载荷配置")
                                     .arg(tpl->requiredPayload));
        }
    }

    QString error;
    const QJsonArray stagesJson = instantiate(name, paramValues, error);
    if (stagesJson.isEmpty()) {
        emit libraryFeedback(tr("模板执行失败：%1").arg(error));
        return false;
    }

    QStringList warnings;
    const QList<SwarmTaskController::Stage> stages = _swarmManager->taskController()->parseStages(
                stagesJson, QGeoCoordinate(mapCenterLat, mapCenterLon), warnings);
    for (const QString &w : warnings) {
        emit libraryFeedback(w);
    }
    if (stages.isEmpty()) {
        emit libraryFeedback(tr("模板执行失败：没有有效阶段"));
        return false;
    }
    return _swarmManager->taskController()->startTask(name, stages);
}

bool SwarmTaskLibrary::saveTemplate(const QString &name, const QString &description,
                                    const QJsonArray &stages, const QJsonArray &parameters)
{
    if (name.trimmed().isEmpty() || stages.isEmpty()) {
        emit libraryFeedback(tr("保存失败：模板名或阶段为空"));
        return false;
    }
    QJsonObject obj;
    obj[QStringLiteral("name")]        = name;
    obj[QStringLiteral("description")] = description;
    obj[QStringLiteral("version")]     = 1;
    obj[QStringLiteral("parameters")]  = parameters;
    obj[QStringLiteral("stages")]      = stages;

    QString fileName = name;
    fileName.replace(QLatin1Char('/'), QLatin1Char('_')).replace(QLatin1Char('\\'), QLatin1Char('_'));
    QFile file(_templateDir + QLatin1Char('/') + fileName + QStringLiteral(".json"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit libraryFeedback(tr("保存失败：无法写入 %1").arg(file.fileName()));
        return false;
    }
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    _scan();
    emit libraryFeedback(tr("模板「%1」已保存").arg(name));
    return true;
}

bool SwarmTaskLibrary::deleteTemplate(const QString &name)
{
    const TaskTemplate *tpl = _find(name);
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
