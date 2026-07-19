#include "SwarmAIAgent.h"

#include "SwarmManager.h"
#include "SwarmAuditLogger.h"
#include "SwarmInputFilter.h"
#include "FormationController.h"
#include "SwarmMissionPlanner.h"
#include "SwarmTaskController.h"
#include "SwarmTaskLibrary.h"
#include "MultiVehicleManager.h"
#include "Vehicle.h"
#include "VehicleFactGroup.h"
#include "FactGroup.h"
#include "Fact.h"
#include "QmlObjectListModel.h"
#include "QGCApplication.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace {
    constexpr int kRequestTimeoutMs   = 60000;
    constexpr int kMaxHistoryMessages = 12;     ///< 保留的多轮对话条数（不含 system）

    QString conditionKindText(const QString &kind)
    {
        static const QMap<QString, QString> texts = {
            {QStringLiteral("all_airborne"),         QStringLiteral("全部升空")},
            {QStringLiteral("all_disarmed"),         QStringLiteral("全部上锁")},
            {QStringLiteral("all_mission_finished"), QStringLiteral("全部完成航线")},
            {QStringLiteral("formation_running"),    QStringLiteral("编队就位")},
            {QStringLiteral("duration"),             QStringLiteral("计时等待")},
            {QStringLiteral("manual"),               QStringLiteral("人工确认")}
        };
        return texts.value(kind, kind);
    }
}

SwarmAIAgent::SwarmAIAgent(SwarmManager *swarmManager)
    : QObject(swarmManager)
    , _swarmManager(swarmManager)
    , _nam(new QNetworkAccessManager(this))
{
    // 接口配置持久化（默认 DeepSeek，兼容 Kimi/通义千问/OpenAI/Ollama 等 OpenAI 格式端点）
    QSettings settings;
    _apiBaseUrl = settings.value(QStringLiteral("SwarmAI/apiBaseUrl"), QStringLiteral("https://api.deepseek.com/v1")).toString();
    _apiKey     = settings.value(QStringLiteral("SwarmAI/apiKey")).toString();
    _modelName  = settings.value(QStringLiteral("SwarmAI/modelName"),  QStringLiteral("deepseek-chat")).toString();
    _useFunctionCalling = settings.value(QStringLiteral("SwarmAI/useFunctionCalling"), true).toBool();

    (void) connect(this, &SwarmAIAgent::agentFeedback, qgcApp(), &QGCApplication::showAppMessage);

    // WP3.5：周期态势摘要定时器（默认关，面板可开）
    _situationTimer = new QTimer(this);
    (void) connect(_situationTimer, &QTimer::timeout, this, &SwarmAIAgent::_sendSituationSummary);
}

void SwarmAIAgent::setSituationSummaryEnabled(bool enabled)
{
    if (_situationSummaryEnabled == enabled) {
        return;
    }
    _situationSummaryEnabled = enabled;
    if (enabled) {
        _situationTimer->start(qMax(10, _situationIntervalSec) * 1000);
    } else {
        _situationTimer->stop();
    }
    emit situationSummaryChanged();
}

void SwarmAIAgent::setSituationIntervalSec(int sec)
{
    sec = qBound(10, sec, 3600);
    if (_situationIntervalSec == sec) {
        return;
    }
    _situationIntervalSec = sec;
    if (_situationSummaryEnabled) {
        _situationTimer->start(sec * 1000);
    }
    emit situationSummaryChanged();
}

SwarmAIAgent::~SwarmAIAgent() = default;

void SwarmAIAgent::setApiBaseUrl(const QString &url)
{
    if (_apiBaseUrl != url) {
        _apiBaseUrl = url;
        QSettings().setValue(QStringLiteral("SwarmAI/apiBaseUrl"), url);
        emit apiBaseUrlChanged();
    }
}

void SwarmAIAgent::setApiKey(const QString &key)
{
    if (_apiKey != key) {
        _apiKey = key;
        QSettings().setValue(QStringLiteral("SwarmAI/apiKey"), key);
        emit apiKeyChanged();
    }
}

void SwarmAIAgent::setModelName(const QString &model)
{
    if (_modelName != model) {
        _modelName = model;
        QSettings().setValue(QStringLiteral("SwarmAI/modelName"), model);
        emit modelNameChanged();
    }
}

void SwarmAIAgent::setUseFunctionCalling(bool use)
{
    if (_useFunctionCalling != use) {
        _useFunctionCalling = use;
        QSettings().setValue(QStringLiteral("SwarmAI/useFunctionCalling"), use);
        emit useFunctionCallingChanged();
    }
}

void SwarmAIAgent::setRequireConfirm(bool require)
{
    if (_requireConfirm != require) {
        _requireConfirm = require;
        emit requireConfirmChanged();
    }
}

QStringList SwarmAIAgent::planSummary() const
{
    QStringList summary;
    if (_pendingTask) {
        summary.append(tr("任务「%1」（%2 个阶段）").arg(_pendingTaskName).arg(_pendingStages.count()));
        for (int i = 0; i < _pendingStages.count(); i++) {
            const SwarmTaskController::Stage &stage = _pendingStages.at(i);
            QStringList actionTexts;
            for (const SwarmAction &action : stage.actions) {
                actionTexts.append(action.description);
            }
            summary.append(tr("阶段 %1「%2」：%3（完成条件：%4）")
                               .arg(i + 1).arg(stage.name, actionTexts.join(QStringLiteral("；")),
                                               conditionKindText(stage.conditionKind)));
        }
    } else {
        for (const SwarmAction &action : _pendingPlan) {
            summary.append(action.description);
        }
    }
    return summary;
}

bool SwarmAIAgent::planHasDangerous() const
{
    if (_pendingTask) {
        for (const SwarmTaskController::Stage &stage : _pendingStages) {
            for (const SwarmAction &action : stage.actions) {
                if (action.dangerous) {
                    return true;
                }
            }
        }
        return false;
    }
    for (const SwarmAction &action : _pendingPlan) {
        if (action.dangerous) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Function Calling 模式：系统提示词只讲规则，输出格式由 tools 定义承担
// ---------------------------------------------------------------------------
QString SwarmAIAgent::_systemPromptTools() const
{
    return QStringLiteral(
        "你是无人机集群地面站的智能指挥助手。操作员用自然语言下达集群任务，你通过调用提供的工具（函数）把意图转化为动作序列。\n\n"
        "【输入】每条用户消息末尾附 [当前态势] JSON：vehicles（在线无人机：id/armed/flight_mode/alt_rel_m/"
        "ground_speed_ms/battery_pct/lat/lon）、selected_vehicle_ids（当前选中分组）、formation（编队状态）、"
        "task（多阶段任务状态）、map_center（当前地图中心经纬度）。\n\n"
        "【规则】\n"
        "1. 只能使用 [当前态势] 中存在的无人机 id。\n"
        "2. 用户指定机号时：先调用 set_selection 再调用后续动作；未指定则不加（作用于当前选中集，为空即全体）。\n"
        "3. 参数限值：altitude 2~120 米，spacing 2~500 米，length/width 10~100000 米，angle 0~360 度。超限取边界值并说明。\n"
        "4. “搜索/巡查/覆盖区域”→ plan_coverage；未给中心时省略 center_lat/center_lon（默认地图中心）。\n"
        "5. 多步任务按顺序依次调用工具，如“全体起飞后楔形编队”→ takeoff + start_formation；"
        "“2号3号返航”→ set_selection([2,3]) + rtl。\n"
        "6. 包含明显先后依赖、需要等待完成再进入下一步的任务（如“起飞→搜索→返航”整体流程）："
        "用 start_swarm_task 一次下发多阶段任务，状态机会监控完成条件自动推进。\n"
        "7. 用户意图与 templates 列表中某个任务模板匹配时：优先调用 run_task_template 按名引用并给出参数，"
        "未指定的参数用模板默认值；模板不完全匹配时才用 start_swarm_task 自定义编排。\n"
        "8. 用户要求“给几个方案/候选/备选”，或任务有多种明显不同的合理实现（如不同队形、不同分区、不同模板）时："
        "调用 propose_candidates 一次给出 2~3 个候选方案（每个含 title/summary/pros/cons，"
        "及 actions 或 template_name+template_params 二选一），由操作员对比选择后执行；否则直接调用执行工具。\n"
        "9. 意图不清或缺关键参数且无法合理默认时：不调用工具，用文字追问。\n"
        "10. 数值必须是数字不带单位。vertical_step 缺省 3，编队 spacing 缺省 10；"
        "plan_coverage 的 assign 缺省 sequential，用户要求“最优分配/就近分配”时用 auction。\n"
        "11. 态势类请求（摘要/问答/告警处置建议）：仅依据 [态势摘要] 中给出的事实作答，"
        "每个事实性陈述必须标注来源角标——[遥测]（机体状态）、[感知]（检测/融合航迹）、"
        "[推测]（规则推导）；摘要中没有的数字不得编造，不确定就说“暂无数据”。"
    );
}

// ---------------------------------------------------------------------------
// JSON 模式（回退）：输出格式由提示词约定
// ---------------------------------------------------------------------------
QString SwarmAIAgent::_systemPromptJson() const
{
    return QStringLiteral(
        "你是无人机集群地面站的智能指挥助手。操作员用自然语言下达集群任务，你负责理解意图，输出结构化动作序列。\n\n"
        "【输入】每条用户消息末尾附 [当前态势] JSON：vehicles、selected_vehicle_ids、formation、task、map_center（含义同字面）。\n\n"
        "【输出】只输出一个 JSON 对象，禁止输出其他任何文字、解释或 markdown 标记：\n"
        "{\"reply\":\"给操作员的简短中文回复\",\"actions\":[动作对象, ...]}\n\n"
        "【动作集】（按数组顺序依次执行）\n"
        "- {\"type\":\"set_selection\",\"vehicle_ids\":[整数,...]} 选中分组，后续集群指令只作用于这些飞机\n"
        "- {\"type\":\"arm\",\"arm\":true/false} 解锁/上锁\n"
        "- {\"type\":\"takeoff\",\"altitude\":米} 集群起飞到相对高度\n"
        "- {\"type\":\"land\"} / {\"type\":\"rtl\"} / {\"type\":\"pause\"} / {\"type\":\"start_mission\"} / {\"type\":\"disarm\"}\n"
        "- {\"type\":\"emergency_disarm\"} 强制上锁（仅当用户明确说“紧急/强制”时使用）\n"
        "- {\"type\":\"start_formation\",\"leader_id\":长机id,\"formation\":\"wedge|column|line|circle\","
        "\"spacing\":间距米,\"vertical_step\":高度分层米} 开始编队\n"
        "- {\"type\":\"stop_formation\"} 停止编队\n"
        "- {\"type\":\"plan_coverage\",\"center\":\"map_center\" 或 [纬度,经度],\"length\":沿航线长米,"
        "\"width\":宽米,\"angle\":航线方位角度,\"spacing\":行距米,\"altitude\":作业高度米} 矩形区域覆盖任务规划并上传\n"
        "- {\"type\":\"clear_missions\"} 清空各机任务\n"
        "- {\"type\":\"start_swarm_task\",\"task_name\":\"名称\",\"stages\":[{\"name\":\"阶段名\","
        "\"actions\":[动作对象,...],\"complete_when\":{\"kind\":\"all_airborne|all_disarmed|all_mission_finished|"
        "formation_running|duration|manual\",\"min_altitude\":米,\"seconds\":秒,\"timeout_sec\":秒}}]} 多阶段任务\n"
        "- {\"type\":\"abort_swarm_task\"} 中止正在运行的多阶段任务\n"
        "- {\"type\":\"run_task_template\",\"name\":\"模板名\",\"parameter_values\":{\"参数名\":值,...}} 引用任务模板（模板清单见 [当前态势] templates）\n\n"
        "【规则】\n"
        "1. 只能使用 [当前态势] 中存在的无人机 id。\n"
        "2. 用户指定机号时：先 set_selection 再下后续指令；未指定则不加。\n"
        "3. 参数限值：altitude 2~120，spacing 2~500，length/width 10~100000，angle 0~360。超限取边界值并说明。\n"
        "4. “搜索/巡查/覆盖区域”→ plan_coverage；未给中心时 center 用 \"map_center\"。\n"
        "5. 有先后依赖的完整流程（起飞→搜索→返航）→ 优先匹配 templates 模板用 run_task_template，不匹配才用 start_swarm_task 自定义编排。\n"
        "6. 意图不清或缺关键参数且无法合理默认时：actions 输出空数组，在 reply 中追问。\n"
        "7. 数值必须是数字不带单位。\n"
        "8. 态势类请求仅依据 [态势摘要] 作答，事实须标注来源角标（[遥测]/[感知]/[推测]），禁止编造。\n\n"
        "【示例】\n"
        "用户：“三架都起飞到30米” → {\"reply\":\"收到，全体起飞至30米。\",\"actions\":[{\"type\":\"takeoff\",\"altitude\":30}]}\n"
        "用户：“1号当长机，其余楔形编队跟住，间距15” → {\"reply\":\"以1号为长机组织楔形编队。\",\"actions\":[{\"type\":\"start_formation\",\"leader_id\":1,\"formation\":\"wedge\",\"spacing\":15,\"vertical_step\":3}]}\n"
        "用户：“让2号先回来” → {\"reply\":\"2号机返航，其余保持。\",\"actions\":[{\"type\":\"set_selection\",\"vehicle_ids\":[2]},{\"type\":\"rtl\"}]}\n"
        "用户：“全体起飞到30米，搜一遍当前区域200乘150行距20，完成后返航” → {\"reply\":\"已编排三阶段任务：起飞→区域搜索→返航，状态机自动推进。\",\"actions\":[{\"type\":\"start_swarm_task\",\"task_name\":\"搜索后返航\",\"stages\":[{\"name\":\"起飞\",\"actions\":[{\"type\":\"takeoff\",\"altitude\":30}],\"complete_when\":{\"kind\":\"all_airborne\",\"min_altitude\":25}},{\"name\":\"搜索\",\"actions\":[{\"type\":\"plan_coverage\",\"center\":\"map_center\",\"length\":200,\"width\":150,\"angle\":0,\"spacing\":20,\"altitude\":30},{\"type\":\"start_mission\"}],\"complete_when\":{\"kind\":\"all_mission_finished\"}},{\"name\":\"返航\",\"actions\":[{\"type\":\"rtl\"}],\"complete_when\":{\"kind\":\"all_disarmed\"}}]}]}"
    );
}

// ---------------------------------------------------------------------------
// OpenAI tools 定义（Function Calling）：13 个动作 + 2 个任务编排
// 参数 Schema 用 JSON 文本声明（QJsonDocument 解析），可读性高且避免嵌套笔误。
// ---------------------------------------------------------------------------
QJsonArray SwarmAIAgent::_toolsJson() const
{
    auto fn = [](const QString &name, const QString &description, const QJsonObject &parameters) {
        return QJsonObject{
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"), QJsonObject{
                {QStringLiteral("name"), name},
                {QStringLiteral("description"), description},
                {QStringLiteral("parameters"), parameters}
            }}
        };
    };
    auto params = [](const QByteArray &json) {
        return QJsonDocument::fromJson(json).object();
    };
    const QJsonObject noParams = params(R"({"type":"object","properties":{}})");

    QJsonArray tools;
    tools.append(fn(QStringLiteral("set_selection"),
                    QStringLiteral("选中无人机分组：后续集群指令只作用于这些飞机"),
                    params(R"({"type":"object","properties":{
                        "vehicle_ids":{"type":"array","items":{"type":"integer"},"description":"在线无人机系统 id 列表"}
                    },"required":["vehicle_ids"]})")));
    tools.append(fn(QStringLiteral("arm"),
                    QStringLiteral("集群解锁（arm=true）或上锁（arm=false）"),
                    params(R"({"type":"object","properties":{
                        "arm":{"type":"boolean"}
                    },"required":["arm"]})")));
    tools.append(fn(QStringLiteral("takeoff"),
                    QStringLiteral("集群起飞到相对高度（米）"),
                    params(R"({"type":"object","properties":{
                        "altitude":{"type":"number","minimum":2,"maximum":120}
                    },"required":["altitude"]})")));
    tools.append(fn(QStringLiteral("land"),           QStringLiteral("集群原地降落"), noParams));
    tools.append(fn(QStringLiteral("rtl"),            QStringLiteral("集群返航"), noParams));
    tools.append(fn(QStringLiteral("pause"),          QStringLiteral("集群暂停悬停"), noParams));
    tools.append(fn(QStringLiteral("start_mission"),  QStringLiteral("集群开始已上传的航线任务"), noParams));
    tools.append(fn(QStringLiteral("disarm"),         QStringLiteral("集群上锁"), noParams));
    tools.append(fn(QStringLiteral("emergency_disarm"),
                    QStringLiteral("紧急强制上锁（飞行中会坠机，仅当用户明确说“紧急/强制”时使用）"), noParams));
    tools.append(fn(QStringLiteral("start_formation"),
                    QStringLiteral("开始长机-僚机编队：wedge楔形/column纵队/line横队/circle环形"),
                    params(R"({"type":"object","properties":{
                        "leader_id":{"type":"integer","description":"长机系统 id"},
                        "formation":{"type":"string","enum":["wedge","column","line","circle"]},
                        "spacing":{"type":"number","description":"队形间距（米），缺省 10"},
                        "vertical_step":{"type":"number","description":"高度分层（米），缺省 3"}
                    },"required":["leader_id","formation"]})")));
    tools.append(fn(QStringLiteral("stop_formation"), QStringLiteral("停止编队，僚机原地悬停"), noParams));
    tools.append(fn(QStringLiteral("plan_coverage"),
                    QStringLiteral("矩形区域覆盖任务规划并上传（不改变当前飞行状态）；缺省中心为当前地图中心"),
                    params(R"({"type":"object","properties":{
                        "center_lat":{"type":"number"},
                        "center_lon":{"type":"number"},
                        "length":{"type":"number","description":"区域沿航线方向长度（米）"},
                        "width":{"type":"number","description":"区域宽度（米）"},
                        "angle":{"type":"number","description":"航线方位角（度），缺省 0"},
                        "spacing":{"type":"number","description":"行距（米），缺省 20"},
                        "altitude":{"type":"number","description":"作业高度（米），缺省 30"},
                        "assign":{"type":"string","enum":["sequential","auction"],"description":"条带分配方式：sequential=按机号升序（缺省）；auction=拍卖法，总转场里程最优"}
                    },"required":["length","width"]})")));
    tools.append(fn(QStringLiteral("clear_missions"), QStringLiteral("清空各机任务"), noParams));

    // 多阶段任务编排
    tools.append(fn(QStringLiteral("start_swarm_task"),
                    QStringLiteral("创建并启动多阶段集群任务：状态机监控每阶段完成条件（含航线进度 MISSION_CURRENT），自动推进"),
                    params(R"({"type":"object","properties":{
                        "task_name":{"type":"string"},
                        "stages":{"type":"array","items":{
                            "type":"object","properties":{
                                "name":{"type":"string"},
                                "actions":{"type":"array","items":{
                                    "type":"object",
                                    "properties":{"type":{"type":"string","enum":[
                                        "set_selection","arm","takeoff","land","rtl","pause",
                                        "start_mission","disarm","start_formation","stop_formation",
                                        "plan_coverage","clear_missions"]}},
                                    "required":["type"],
                                    "description":"动作对象：type 加对应参数（同各工具参数）"}},
                                "complete_when":{"type":"object","properties":{
                                    "kind":{"type":"string","enum":[
                                        "all_airborne","all_disarmed","all_mission_finished",
                                        "formation_running","duration","manual"]},
                                    "min_altitude":{"type":"number"},
                                    "seconds":{"type":"number"},
                                    "timeout_sec":{"type":"number"}},
                                 "required":["kind"]}},
                            "required":["name","actions","complete_when"]}}
                    },"required":["task_name","stages"]})")));
    tools.append(fn(QStringLiteral("abort_swarm_task"), QStringLiteral("中止正在运行的多阶段任务（全体暂停悬停）"), noParams));
    tools.append(fn(QStringLiteral("run_task_template"),
                    QStringLiteral("按名引用任务模板库中的多阶段任务模板并代入参数执行（模板清单见上下文 templates），优于 start_swarm_task 自定义编排"),
                    params(R"({"type":"object","properties":{
                        "name":{"type":"string","description":"模板名（须来自上下文 templates 列表）"},
                        "parameter_values":{"type":"object","description":"模板参数键值对，缺省用模板默认值"}
                    },"required":["name"]})")));
    tools.append(fn(QStringLiteral("propose_candidates"),
                    QStringLiteral("给出 2~3 个候选任务方案供操作员对比选择（WP2.2；仅在用户要求多方案或存在多种明显不同实现时使用）"),
                    params(R"({"type":"object","properties":{
                        "candidates":{"type":"array","minItems":2,"maxItems":3,"items":{"type":"object","properties":{
                            "title":{"type":"string","description":"方案名"},
                            "summary":{"type":"string","description":"一句话概述"},
                            "pros":{"type":"string","description":"优点"},
                            "cons":{"type":"string","description":"缺点"},
                            "actions":{"type":"array","description":"动作序列（与执行动作同构，如 {\"type\":\"takeoff\",\"altitude\":30}）","items":{"type":"object"}},
                            "template_name":{"type":"string","description":"或：引用任务模板名（与 actions 二选一）"},
                            "template_params":{"type":"object","description":"模板参数"}
                        },"required":["title","summary"]}}
                    },"required":["candidates"]})")));
    return tools;
}

// ---------------------------------------------------------------------------
// 实时集群态势（随每条用户消息注入）
// ---------------------------------------------------------------------------
QJsonObject SwarmAIAgent::_buildSwarmContext(double mapCenterLat, double mapCenterLon) const
{
    MultiVehicleManager *mvm = MultiVehicleManager::instance();

    QJsonArray vehiclesJson;
    QmlObjectListModel *vehicles = mvm->vehicles();
    for (int i = 0; i < vehicles->count(); i++) {
        Vehicle *vehicle = qobject_cast<Vehicle *>(vehicles->get(i));
        if (!vehicle) {
            continue;
        }
        VehicleFactGroup *vfg = qobject_cast<VehicleFactGroup *>(vehicle->vehicleFactGroup());

        double batteryPct = -1.0;
        if (vehicle->batteries()->count() > 0) {
            FactGroup *bg = qobject_cast<FactGroup *>(vehicle->batteries()->get(0));
            if (bg && bg->getFact(QStringLiteral("percentRemaining"))) {
                batteryPct = bg->getFact(QStringLiteral("percentRemaining"))->rawValue().toDouble();
            }
        }

        QJsonObject v;
        v[QStringLiteral("id")]             = vehicle->id();
        v[QStringLiteral("armed")]          = vehicle->armed();
        v[QStringLiteral("flight_mode")]    = vehicle->flightMode();
        v[QStringLiteral("alt_rel_m")]      = vfg ? vfg->altitudeRelative()->rawValue().toDouble() : 0.0;
        v[QStringLiteral("ground_speed_ms")]= vfg ? vfg->groundSpeed()->rawValue().toDouble() : 0.0;
        v[QStringLiteral("battery_pct")]    = batteryPct;
        v[QStringLiteral("lat")]            = vehicle->coordinate().latitude();
        v[QStringLiteral("lon")]            = vehicle->coordinate().longitude();
        vehiclesJson.append(v);
    }

    QJsonArray selectedJson;
    for (int i = 0; i < mvm->selectedVehicles()->count(); i++) {
        Vehicle *vehicle = qobject_cast<Vehicle *>(mvm->selectedVehicles()->get(i));
        if (vehicle) {
            selectedJson.append(vehicle->id());
        }
    }

    FormationController *formation = _swarmManager->formation();
    static const QStringList typeNames = {
        QStringLiteral("wedge"), QStringLiteral("column"), QStringLiteral("line"), QStringLiteral("circle")
    };
    QJsonObject formationJson;
    formationJson[QStringLiteral("active")]          = formation->active();
    formationJson[QStringLiteral("leader_id")]       = formation->leaderVehicleId();
    formationJson[QStringLiteral("type")]            = typeNames.at(static_cast<int>(formation->formationType()));
    formationJson[QStringLiteral("spacing_m")]       = formation->spacingMeters();
    formationJson[QStringLiteral("vertical_step_m")] = formation->verticalStepMeters();
    formationJson[QStringLiteral("follower_count")]  = formation->followerCount();

    SwarmTaskController *task = _swarmManager->taskController();
    QJsonObject taskJson;
    taskJson[QStringLiteral("running")]             = task->running();
    taskJson[QStringLiteral("task_name")]           = task->taskName();
    taskJson[QStringLiteral("current_stage_index")] = task->currentStageIndex();
    taskJson[QStringLiteral("stage_count")]         = task->stageCount();
    taskJson[QStringLiteral("state")]               = task->stateText();

    QJsonObject mapCenterJson;
    mapCenterJson[QStringLiteral("lat")] = mapCenterLat;
    mapCenterJson[QStringLiteral("lon")] = mapCenterLon;

    QJsonObject ctx;
    ctx[QStringLiteral("map_center")]           = mapCenterJson;
    ctx[QStringLiteral("vehicle_count")]        = vehicles->count();
    ctx[QStringLiteral("vehicles")]             = vehiclesJson;
    ctx[QStringLiteral("selected_vehicle_ids")] = selectedJson;
    ctx[QStringLiteral("formation")]            = formationJson;
    ctx[QStringLiteral("task")]                 = taskJson;
    ctx[QStringLiteral("templates")]            = _swarmManager->taskLibrary()->contextSummary();

    // WP3.5：感知/告警摘要随每轮对话下发（事实块带 source，规则 11 强制来源标注）
    const QJsonDocument digestDoc = QJsonDocument::fromJson(_swarmManager->situationDigestJson().toUtf8());
    if (digestDoc.isObject()) {
        const QJsonObject digest = digestDoc.object();
        QJsonObject sit;
        sit[QStringLiteral("one_liner")] = _swarmManager->situationOneLiner();
        sit[QStringLiteral("tracks")]    = digest.value(QStringLiteral("tracks"));
        sit[QStringLiteral("alerts")]    = digest.value(QStringLiteral("alerts"));
        ctx[QStringLiteral("situation")] = sit;
    }
    return ctx;
}

// ---------------------------------------------------------------------------
// 发送与接收
// ---------------------------------------------------------------------------
void SwarmAIAgent::sendText(const QString &text, double mapCenterLat, double mapCenterLon)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    if (_busy) {
        emit agentFeedback(tr("上一条指令正在处理，请稍候"));
        return;
    }
    if (_apiKey.trimmed().isEmpty()) {
        emit agentFeedback(tr("请先在 AI 面板配置 API Key"));
        _appendChat(tr("系统"), tr("未配置 API Key，无法请求大模型。"));
        return;
    }

    // 防注入过滤（WP1.2）：拦截级拒发，可疑级放行但告警 + 审计
    const SwarmInputFilter::Verdict verdict = SwarmInputFilter::instance()->check(trimmed, QStringLiteral("operator"));
    SwarmAuditLogger::instance()->logUserInput(trimmed, verdict.level);
    if (SwarmInputFilter::isBlocked(verdict)) {
        const QString msg = tr("输入被安全过滤拦截（命中：%1），未发送到大模型。").arg(verdict.matched.join(QStringLiteral(", ")));
        _appendChat(tr("系统"), msg);
        emit agentFeedback(msg);
        return;
    }
    if (SwarmInputFilter::isSuspicious(verdict)) {
        _appendChat(tr("系统"), tr("提示：输入包含可疑模式（%1），已记录审计并继续。").arg(verdict.matched.join(QStringLiteral(", "))));
    }
    const QString cleaned = verdict.sanitized;

    _lastMapCenterLat = mapCenterLat;
    _lastMapCenterLon = mapCenterLon;
    _appendChat(tr("你"), trimmed);

    // 用户消息 = 清洗后的指令 + 实时态势
    const QJsonObject ctx = _buildSwarmContext(mapCenterLat, mapCenterLon);
    QJsonObject userMsg;
    userMsg[QStringLiteral("role")]    = QStringLiteral("user");
    userMsg[QStringLiteral("content")] = QStringLiteral("%1\n\n[当前态势]\n%2")
            .arg(cleaned, QString::fromUtf8(QJsonDocument(ctx).toJson(QJsonDocument::Compact)));
    _history.append(userMsg);
    while (_history.count() > kMaxHistoryMessages) {
        _history.removeFirst();
    }

    _sendRequest();
}

// ---------------------------------------------------------------------------
// WP3.5 态势摘要与问答（事实来源标注防幻觉）
// ---------------------------------------------------------------------------

void SwarmAIAgent::askSituation(const QString &question, double mapCenterLat, double mapCenterLon)
{
    const QString trimmed = question.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    if (_busy) {
        emit agentFeedback(tr("上一条请求正在处理，请稍候"));
        return;
    }
    if (_apiKey.trimmed().isEmpty()) {
        emit agentFeedback(tr("请先在 AI 面板配置 API Key"));
        return;
    }
    _lastMapCenterLat = mapCenterLat;
    _lastMapCenterLon = mapCenterLon;
    _appendChat(tr("你"), trimmed);

    // 态势问答：问题 + 结构化摘要（每个事实块自带 source），规则 11 强制标注来源
    QJsonObject userMsg;
    userMsg[QStringLiteral("role")]    = QStringLiteral("user");
    userMsg[QStringLiteral("content")] = QStringLiteral(
            "%1\n\n[态势摘要]（事实块含 source 字段：telemetry/perception/inferred）\n%2")
            .arg(trimmed, _swarmManager->situationDigestJson());
    _history.append(userMsg);
    while (_history.count() > kMaxHistoryMessages) {
        _history.removeFirst();
    }
    SwarmAuditLogger::instance()->log(QStringLiteral("dialog"), QStringLiteral("situation_query"),
                                      {{QStringLiteral("question"), trimmed}});
    _sendRequest();
}

void SwarmAIAgent::requestAdvice(const QString &alertText)
{
    if (_busy || _apiKey.trimmed().isEmpty()) {
        return;                                 // 忙时丢弃，告警本身已上面板与审计
    }
    QJsonObject userMsg;
    userMsg[QStringLiteral("role")]    = QStringLiteral("user");
    userMsg[QStringLiteral("content")] = QStringLiteral(
            "【紧急告警】%1\n请用不超过 3 句话给出处置建议，可选动作仅限已提供的工具；"
            "事实标注来源角标（[遥测]/[感知]/[推测]）。\n\n[态势摘要]\n%2")
            .arg(alertText, _swarmManager->situationDigestJson());
    _history.append(userMsg);
    while (_history.count() > kMaxHistoryMessages) {
        _history.removeFirst();
    }
    _appendChat(tr("系统"), tr("已为紧急告警请求处置建议…"));
    SwarmAuditLogger::instance()->log(QStringLiteral("dialog"), QStringLiteral("alert_advice_request"),
                                      {{QStringLiteral("alert"), alertText}});
    _sendRequest();
}

void SwarmAIAgent::_sendSituationSummary()
{
    if (_busy || _apiKey.trimmed().isEmpty()) {
        return;                                 // 本轮跳过，下个周期再试
    }
    QJsonObject userMsg;
    userMsg[QStringLiteral("role")]    = QStringLiteral("user");
    userMsg[QStringLiteral("content")] = QStringLiteral(
            "【周期态势】请基于以下结构化摘要生成不超过 3 句的自然语言态势简报，"
            "突出变化与风险，事实标注来源角标（[遥测]/[感知]/[推测]），"
            "禁止编造摘要中不存在的数字。\n\n[态势摘要]\n%1")
            .arg(_swarmManager->situationDigestJson());
    _history.append(userMsg);
    while (_history.count() > kMaxHistoryMessages) {
        _history.removeFirst();
    }
    SwarmAuditLogger::instance()->log(QStringLiteral("dialog"), QStringLiteral("situation_summary_tick"), {});
    _sendRequest();
}

void SwarmAIAgent::_sendRequest()
{
    QJsonArray messages;
    QJsonObject systemMsg;
    systemMsg[QStringLiteral("role")]    = QStringLiteral("system");
    systemMsg[QStringLiteral("content")] = _useFunctionCalling ? _systemPromptTools() : _systemPromptJson();
    messages.append(systemMsg);
    for (const QJsonObject &msg : _history) {
        messages.append(msg);
    }

    QJsonObject body;
    body[QStringLiteral("model")]       = _modelName;
    body[QStringLiteral("messages")]    = messages;
    body[QStringLiteral("temperature")] = 0.2;
    if (_useFunctionCalling) {
        body[QStringLiteral("tools")]       = _toolsJson();
        body[QStringLiteral("tool_choice")] = QStringLiteral("auto");
    } else {
        // JSON 输出模式（DeepSeek/Kimi/Qwen/Ollama 均兼容；不支持时由解析端容错提取）
        body[QStringLiteral("response_format")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("json_object")}};
    }

    QNetworkRequest request(QUrl(_apiBaseUrl + QStringLiteral("/chat/completions")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + _apiKey.toUtf8());
    request.setTransferTimeout(kRequestTimeoutMs);

    SwarmAuditLogger::instance()->logLlmRequest(_modelName, _useFunctionCalling, _history.count());
    _requestStartMs = QDateTime::currentMSecsSinceEpoch();      // WP2.6 时延埋点
    _setBusy(true);
    _reply = _nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));

    QNetworkReply *reply = _reply;
    (void) connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (_reply == reply) {
            _reply = nullptr;
        }
        _setBusy(false);

        if (reply->error() != QNetworkReply::NoError) {
            QString errMsg = tr("大模型请求失败：%1（HTTP %2）")
                    .arg(reply->errorString())
                    .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
            if (_useFunctionCalling
                    && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 400) {
                errMsg += tr("；该端点可能不支持 Function Calling，请在配置中关闭“工具调用”");
            }
            emit agentFeedback(errMsg);
            _appendChat(tr("系统"), errMsg);
            return;
        }
        _handleResponse(reply->readAll());
    });
}

void SwarmAIAgent::_handleResponse(const QByteArray &body)
{
    // WP2.6 时延埋点：意图 → 方案响应耗时（验收 ≤30s）
    if (_requestStartMs > 0) {
        _responseTimeSec = (QDateTime::currentMSecsSinceEpoch() - _requestStartMs) / 1000.0;
        _requestStartMs = 0;
        emit responseTimeSecChanged();
        SwarmAuditLogger::instance()->log(QStringLiteral("dialog"), QStringLiteral("llm_latency"),
                                          {{QStringLiteral("ms"), QJsonValue(static_cast<qint64>(_responseTimeSec * 1000))}});
    }

    const QJsonDocument outerDoc = QJsonDocument::fromJson(body);
    if (!outerDoc.isObject()) {
        emit agentFeedback(tr("大模型返回格式异常（非 JSON 响应）"));
        return;
    }
    const QJsonObject message = outerDoc.object()
            .value(QStringLiteral("choices")).toArray().first().toObject()
            .value(QStringLiteral("message")).toObject();

    QString replyText = message.value(QStringLiteral("content")).toString();

    // ---------- 路径 A：Function Calling（tool_calls） ----------
    const QJsonArray toolCalls = message.value(QStringLiteral("tool_calls")).toArray();
    if (!toolCalls.isEmpty()) {
        QJsonArray actionsJson;             // 基础动作统一转成 JSON 动作格式，走统一管线
        QStringList immediateTexts;
        for (const QJsonValue &callVal : toolCalls) {
            const QJsonObject fn = callVal.toObject()
                    .value(QStringLiteral("function")).toObject();
            const QString name = fn.value(QStringLiteral("name")).toString();
            const QJsonObject args = QJsonDocument::fromJson(
                        fn.value(QStringLiteral("arguments")).toString().toUtf8()).object();

            if (name == QStringLiteral("start_swarm_task")) {
                QString taskName;
                QList<SwarmTaskController::Stage> stages;
                QStringList warnings;
                if (_handleTaskJson(args, taskName, stages, warnings)) {
                    _pendingTask     = true;
                    _pendingTaskName = taskName;
                    _pendingStages   = stages;
                }
                for (const QString &w : warnings) { _appendChat(tr("系统"), w); }
            } else if (name == QStringLiteral("run_task_template")) {
                _runTaskTemplate(args);
            } else if (name == QStringLiteral("propose_candidates")) {
                _handleCandidates(args);
            } else if (name == QStringLiteral("abort_swarm_task")) {
                _swarmManager->taskController()->abort();
            } else {
                QJsonObject action = args;
                action[QStringLiteral("type")] = name;
                if (name == QStringLiteral("plan_coverage")
                        && args.contains(QStringLiteral("center_lat"))
                        && args.contains(QStringLiteral("center_lon"))) {
                    action[QStringLiteral("center")] = QJsonArray{args.value(QStringLiteral("center_lat")).toDouble(),
                                                                  args.value(QStringLiteral("center_lon")).toDouble()};
                }
                actionsJson.append(action);
            }
            immediateTexts.append(name);
        }

        // 历史记录：只存文本摘要，不存 tool_calls（避免缺 tool 响应导致后续请求 400）
        QJsonObject assistantMsg;
        assistantMsg[QStringLiteral("role")]    = QStringLiteral("assistant");
        assistantMsg[QStringLiteral("content")] = replyText + QStringLiteral("\n[已规划动作：%1]").arg(immediateTexts.join(QStringLiteral(", ")));
        _history.append(assistantMsg);
        if (!replyText.isEmpty()) {
            _appendChat(tr("AI"), replyText);
        }
        SwarmAuditLogger::instance()->logLlmOutput(replyText, immediateTexts);

        if (!actionsJson.isEmpty()) {
            QStringList warnings;
            const QList<SwarmAction> plan = _swarmManager->parseAndValidate(
                        actionsJson, QGeoCoordinate(_lastMapCenterLat, _lastMapCenterLon), warnings);
            SwarmAuditLogger::instance()->logValidation(actionsJson.count(), plan.count(), warnings);
            for (const QString &w : warnings) { _appendChat(tr("系统"), w); }
            _queueOrExecute(plan);
        }
        if (_pendingTask) {
            emit planPendingChanged();
            emit planChanged();
            emit agentFeedback(tr("AI 已生成多阶段任务计划，请在面板确认执行"));
        }
        return;
    }

    // ---------- 路径 B：JSON 文本模式（回退） ----------
    QString jsonText = replyText;
    const int firstBrace = replyText.indexOf(QLatin1Char('{'));
    const int lastBrace  = replyText.lastIndexOf(QLatin1Char('}'));
    if (firstBrace >= 0 && lastBrace > firstBrace) {
        jsonText = replyText.mid(firstBrace, lastBrace - firstBrace + 1);
    }
    const QJsonDocument doc = QJsonDocument::fromJson(jsonText.toUtf8());
    if (!doc.isObject()) {
        if (replyText.isEmpty()) {
            emit agentFeedback(tr("大模型返回了空内容"));
        } else {
            // 纯文字回复（追问/说明），直接展示
            _appendChat(tr("AI"), replyText);
            QJsonObject assistantMsg;
            assistantMsg[QStringLiteral("role")]    = QStringLiteral("assistant");
            assistantMsg[QStringLiteral("content")] = replyText;
            _history.append(assistantMsg);
        }
        return;
    }

    const QJsonObject result = doc.object();
    replyText = result.value(QStringLiteral("reply")).toString();

    QJsonObject assistantMsg;
    assistantMsg[QStringLiteral("role")]    = QStringLiteral("assistant");
    assistantMsg[QStringLiteral("content")] = message.value(QStringLiteral("content")).toString();
    _history.append(assistantMsg);

    if (!replyText.isEmpty()) {
        _appendChat(tr("AI"), replyText);
    }
    SwarmAuditLogger::instance()->logLlmOutput(replyText, {});
    _handleActionsJson(result.value(QStringLiteral("actions")).toArray(), replyText);
}

// ---------------------------------------------------------------------------
// JSON 动作数组处理（JSON 模式回退路径）
// ---------------------------------------------------------------------------
void SwarmAIAgent::_handleActionsJson(const QJsonArray &actionsJson, const QString &replyText)
{
    Q_UNUSED(replyText);
    if (actionsJson.isEmpty()) {
        return;
    }

    QJsonArray basicActions;
    for (const QJsonValue &val : actionsJson) {
        const QJsonObject obj = val.toObject();
        const QString type = obj.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("start_swarm_task")) {
            QString taskName;
            QList<SwarmTaskController::Stage> stages;
            QStringList warnings;
            if (_handleTaskJson(obj, taskName, stages, warnings)) {
                _pendingTask     = true;
                _pendingTaskName = taskName;
                _pendingStages   = stages;
            }
            for (const QString &w : warnings) { _appendChat(tr("系统"), w); }
        } else if (type == QStringLiteral("run_task_template")) {
            _runTaskTemplate(obj);
        } else if (type == QStringLiteral("abort_swarm_task")) {
            _swarmManager->taskController()->abort();
        } else {
            basicActions.append(obj);
        }
    }

    if (!basicActions.isEmpty()) {
        QStringList warnings;
        const QList<SwarmAction> plan = _swarmManager->parseAndValidate(
                    basicActions, QGeoCoordinate(_lastMapCenterLat, _lastMapCenterLon), warnings);
        SwarmAuditLogger::instance()->logValidation(basicActions.count(), plan.count(), warnings);
        for (const QString &w : warnings) { _appendChat(tr("系统"), w); }
        _queueOrExecute(plan);
    }
    if (_pendingTask) {
        emit planPendingChanged();
        emit planChanged();
        emit agentFeedback(tr("AI 已生成多阶段任务计划，请在面板确认执行"));
    }
}

// ---------------------------------------------------------------------------
// 多阶段任务 JSON → Stage 列表（动作经统一管线校验）
// ---------------------------------------------------------------------------
bool SwarmAIAgent::_handleTaskJson(const QJsonObject &taskObj, QString taskName,
                                   QList<SwarmTaskController::Stage> &stages, QStringList &warnings)
{
    taskName = taskObj.value(QStringLiteral("task_name")).toString(tr("未命名任务"));
    const QJsonArray stagesJson = taskObj.value(QStringLiteral("stages")).toArray();
    if (stagesJson.isEmpty()) {
        warnings.append(tr("start_swarm_task 缺少 stages，已忽略"));
        return false;
    }
    stages = _swarmManager->taskController()->parseStages(
                stagesJson, QGeoCoordinate(_lastMapCenterLat, _lastMapCenterLon), warnings);
    if (stages.isEmpty()) {
        warnings.append(tr("start_swarm_task 没有有效阶段，已忽略"));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 模板引用：实例化 → 统一校验 → 挂起待确认（人在回路）
// ---------------------------------------------------------------------------
void SwarmAIAgent::_runTaskTemplate(const QJsonObject &args)
{
    const QString templateName = args.value(QStringLiteral("name")).toString();
    const QVariantMap paramValues = args.value(QStringLiteral("parameter_values")).toObject().toVariantMap();

    QString error;
    const QJsonArray stagesJson = _swarmManager->taskLibrary()->instantiate(templateName, paramValues, error);
    if (stagesJson.isEmpty()) {
        _appendChat(tr("系统"), tr("模板引用失败：%1").arg(error));
        emit agentFeedback(tr("模板引用失败：%1").arg(error));
        return;
    }

    SwarmAuditLogger::instance()->logTemplateRun(templateName, paramValues);

    QStringList warnings;
    const QList<SwarmTaskController::Stage> stages = _swarmManager->taskController()->parseStages(
                stagesJson, QGeoCoordinate(_lastMapCenterLat, _lastMapCenterLon), warnings);
    for (const QString &w : warnings) { _appendChat(tr("系统"), w); }
    if (stages.isEmpty()) {
        _appendChat(tr("系统"), tr("模板「%1」没有有效阶段").arg(templateName));
        return;
    }

    _pendingTask     = true;
    _pendingTaskName = templateName;
    _pendingStages   = stages;
    _appendChat(tr("AI"), tr("已按模板「%1」生成 %2 阶段任务计划。").arg(templateName).arg(stages.count()));
}

// ---------------------------------------------------------------------------
// 人在回路分流：危险动作计划挂起待确认，纯安全计划直接执行
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// WP2.2 多候选方案
// ---------------------------------------------------------------------------

void SwarmAIAgent::_handleCandidates(const QJsonObject &args)
{
    const QJsonArray arr = args.value(QStringLiteral("candidates")).toArray();
    if (arr.isEmpty()) {
        _appendChat(tr("系统"), tr("候选方案为空，请直接描述任务"));
        return;
    }
    _candidates.clear();
    _candidatesVariant.clear();

    const int count = qMin(3, arr.count());
    for (int i = 0; i < count; i++) {
        const QJsonObject c = arr.at(i).toObject();
        const QString title   = c.value(QStringLiteral("title")).toString(tr("方案 %1").arg(i + 1));
        const QString summary = c.value(QStringLiteral("summary")).toString();
        const QString pros    = c.value(QStringLiteral("pros")).toString();
        const QString cons    = c.value(QStringLiteral("cons")).toString();

        Candidate cand;
        QList<SwarmAction> flatActions;
        QString kindText;
        if (c.value(QStringLiteral("actions")).isArray()) {
            // 动作型候选：plan_coverage 的 center_lat/lon 转 center 数组
            QJsonArray actionsJson;
            for (const QJsonValue &av : c.value(QStringLiteral("actions")).toArray()) {
                QJsonObject a = av.toObject();
                if (a.value(QStringLiteral("type")).toString() == QStringLiteral("plan_coverage")
                        && (a.contains(QStringLiteral("center_lat")) || a.contains(QStringLiteral("center_lon")))) {
                    a[QStringLiteral("center")] = QJsonArray{a.value(QStringLiteral("center_lat")),
                                                             a.value(QStringLiteral("center_lon"))};
                }
                actionsJson.append(a);
            }
            QStringList warnings;
            cand.plan = _swarmManager->parseAndValidate(
                        actionsJson, QGeoCoordinate(_lastMapCenterLat, _lastMapCenterLon), warnings);
            for (const QString &w : warnings) { _appendChat(tr("系统"), w); }
            flatActions = cand.plan;
            kindText = tr("直接计划");
        } else if (c.contains(QStringLiteral("template_name"))) {
            const QString tplName = c.value(QStringLiteral("template_name")).toString();
            const QVariantMap pv = c.value(QStringLiteral("template_params")).toObject().toVariantMap();
            QString error;
            const QJsonArray stagesJson = _swarmManager->taskLibrary()->instantiate(tplName, pv, error);
            QStringList warnings;
            cand.stages = _swarmManager->taskController()->parseStages(
                        stagesJson, QGeoCoordinate(_lastMapCenterLat, _lastMapCenterLon), warnings);
            for (const QString &w : warnings) { _appendChat(tr("系统"), w); }
            cand.isTask   = true;
            cand.taskName = tplName;
            for (const SwarmTaskController::Stage &s : std::as_const(cand.stages)) {
                flatActions.append(s.actions);
            }
            kindText = tr("模板「%1」").arg(tplName);
        } else {
            continue;   // 无有效内容的候选丢弃
        }

        // 约束检查（WP2.1）与预演估算（WP2.7）
        const QVariantList violations = _swarmManager->checkConstraints(flatActions, _lastMapCenterLat, _lastMapCenterLon);
        const QVariantMap est = _swarmManager->estimatePlan(flatActions, _lastMapCenterLat, _lastMapCenterLon);
        int errorCount = 0;
        for (const QVariant &v : violations) {
            if (v.toMap().value(QStringLiteral("severity")).toString() == QStringLiteral("error")) {
                errorCount++;
            }
        }

        _candidates.append(cand);
        _candidatesVariant.append(QVariantMap{
            {QStringLiteral("index"),       i},
            {QStringLiteral("title"),       title},
            {QStringLiteral("summary"),     summary},
            {QStringLiteral("pros"),        pros},
            {QStringLiteral("cons"),        cons},
            {QStringLiteral("kind"),        kindText},
            {QStringLiteral("estimate"),    est.value(QStringLiteral("summary"))},
            {QStringLiteral("error_count"), errorCount},
            {QStringLiteral("violations"),  violations},
        });
    }

    emit candidatesChanged();
    if (_candidates.isEmpty()) {
        _appendChat(tr("系统"), tr("候选方案均无有效内容，请直接描述任务"));
        return;
    }
    _appendChat(tr("系统"), tr("AI 给出 %1 个候选方案，请在下方对比选择。").arg(_candidates.count()));
    SwarmAuditLogger::instance()->log(QStringLiteral("dialog"), QStringLiteral("candidates_proposed"),
                                      {{QStringLiteral("count"), _candidates.count()}});
    emit agentFeedback(tr("AI 给出 %1 个候选方案，请选择").arg(_candidates.count()));
}

void SwarmAIAgent::selectCandidate(int index)
{
    if (index < 0 || index >= _candidates.count()) {
        return;
    }
    const Candidate cand = _candidates.at(index);
    _candidates.clear();
    _candidatesVariant.clear();
    emit candidatesChanged();
    SwarmAuditLogger::instance()->log(QStringLiteral("dialog"), QStringLiteral("candidate_selected"),
                                      {{QStringLiteral("index"), index}});

    if (cand.isTask) {
        // 任务型候选 → 多阶段任务确认流程（自主等级闸门在 confirmPlan 把关）
        _pendingTask     = true;
        _pendingTaskName = cand.taskName;
        _pendingStages   = cand.stages;
        emit planPendingChanged();
        emit planChanged();
        _appendChat(tr("系统"), tr("已选择候选任务「%1」，请确认执行。").arg(cand.taskName));
        emit agentFeedback(tr("已选择候选任务，请在面板确认执行"));
    } else {
        _queueOrExecute(cand.plan);
    }
}

void SwarmAIAgent::rejectCandidates()
{
    _candidates.clear();
    _candidatesVariant.clear();
    emit candidatesChanged();
    _appendChat(tr("系统"), tr("已放弃全部候选方案。"));
    SwarmAuditLogger::instance()->log(QStringLiteral("dialog"), QStringLiteral("candidates_rejected"));
}

QStringList SwarmAIAgent::pendingPlanParamsJson() const
{
    QStringList out;
    for (const SwarmAction &action : _pendingPlan) {
        out.append(QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(action.params))
                                     .toJson(QJsonDocument::Compact)));
    }
    return out;
}

// ---------------------------------------------------------------------------
// WP2.3 计划可编辑
// ---------------------------------------------------------------------------

void SwarmAIAgent::updatePendingAction(int index, const QString &paramsJson)
{
    if (index < 0 || index >= _pendingPlan.count()) {
        emit agentFeedback(tr("动作序号无效"));
        return;
    }
    const QJsonObject edited = QJsonDocument::fromJson(paramsJson.toUtf8()).object();
    if (edited.isEmpty()) {
        emit agentFeedback(tr("参数 JSON 解析失败，请检查格式"));
        return;
    }
    // 合并编辑参数后重新过统一校验（与 LLM 计划同一管线）
    const SwarmAction &old = _pendingPlan.at(index);
    QJsonObject actionJson;
    actionJson[QStringLiteral("type")] = old.type;
    for (auto it = old.params.constBegin(); it != old.params.constEnd(); ++it) {
        actionJson.insert(it.key(), QJsonValue::fromVariant(it.value()));
    }
    for (auto it = edited.constBegin(); it != edited.constEnd(); ++it) {
        actionJson.insert(it.key(), it.value());
    }
    // plan_coverage 内部存储为 lat/lon，重新校验需转回 center 数组
    if (old.type == QStringLiteral("plan_coverage") && actionJson.contains(QStringLiteral("lat"))) {
        actionJson[QStringLiteral("center")] = QJsonArray{actionJson.value(QStringLiteral("lat")),
                                                          actionJson.value(QStringLiteral("lon"))};
    }
    QStringList warnings;
    const QList<SwarmAction> revalidated = _swarmManager->parseAndValidate(
                QJsonArray{actionJson}, QGeoCoordinate(_lastMapCenterLat, _lastMapCenterLon), warnings);
    if (revalidated.isEmpty()) {
        emit agentFeedback(tr("编辑未生效，参数未通过校验：%1").arg(warnings.join(QStringLiteral("；"))));
        return;
    }
    _pendingPlan[index] = revalidated.first();
    emit planChanged();
    SwarmAuditLogger::instance()->log(QStringLiteral("dialog"), QStringLiteral("plan_edited"), {
        {QStringLiteral("index"), index},
        {QStringLiteral("params"), paramsJson.left(500)}
    });
    _appendChat(tr("系统"), tr("第 %1 项动作参数已更新：%2").arg(index + 1).arg(revalidated.first().description));
}

void SwarmAIAgent::_queueOrExecute(const QList<SwarmAction> &plan)
{
    if (plan.isEmpty()) {
        return;
    }

    // 约束校验闸门（WP2.1）：error 级违例强制挂起人工确认（即使 L3 白名单放行）
    const QVariantList violations = _swarmManager->checkConstraints(plan, _lastMapCenterLat, _lastMapCenterLon);
    int errorCount = 0;
    QStringList violationTexts;
    for (const QVariant &v : violations) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("severity")).toString() == QStringLiteral("error")) {
            errorCount++;
        }
        violationTexts.append(tr("[%1][%2] %3 建议：%4")
                .arg(m.value(QStringLiteral("severity")).toString(),
                     m.value(QStringLiteral("rule")).toString(),
                     m.value(QStringLiteral("message")).toString(),
                     m.value(QStringLiteral("suggestion")).toString()));
    }
    if (!violationTexts.isEmpty()) {
        _appendChat(tr("系统"), tr("约束校验发现 %1 项问题：\n%2")
                        .arg(violationTexts.count()).arg(violationTexts.join(QStringLiteral("\n"))));
    }

    // 解析式预演估算（WP2.7）：确认前展示预计时长/里程/电量裕度
    const QVariantMap est = _swarmManager->estimatePlan(plan, _lastMapCenterLat, _lastMapCenterLon);
    _appendChat(tr("系统"), tr("任务预演估算：%1").arg(est.value(QStringLiteral("summary")).toString()));

    if (errorCount > 0) {
        _pendingPlan = plan;
        emit planPendingChanged();
        emit planChanged();
        const QString reason = tr("约束校验未通过（%1 项违例），需人工确认").arg(errorCount);
        SwarmAuditLogger::instance()->logPlanPending(planSummary(), reason);
        emit agentFeedback(reason);
        return;
    }

    // 自主等级闸门（WP1.3）：L1 一律禁止下发（仅展示）；L2 含危险动作需确认；L3 按白名单
    QString reason;
    if (_swarmManager->aiExecutionBlocked(&reason)) {
        _pendingPlan = plan;
        emit planPendingChanged();
        emit planChanged();
        _appendChat(tr("系统"), tr("%1（切换至 L2/L3 后方可确认下发）").arg(reason));
        SwarmAuditLogger::instance()->logPlanPending(planSummary(), reason);
        emit agentFeedback(reason);
        return;
    }
    if (_requireConfirm && _swarmManager->needsConfirmation(plan, &reason)) {
        _pendingPlan = plan;
        emit planPendingChanged();
        emit planChanged();
        SwarmAuditLogger::instance()->logPlanPending(planSummary(), reason);
        emit agentFeedback(tr("AI 已生成行动计划（含危险动作 %1 项），请在面板确认执行").arg(plan.count()));
    } else {
        _swarmManager->executeActions(plan, QStringLiteral("ai"));
    }
}

void SwarmAIAgent::confirmPlan()
{
    // 自主等级闸门：L1 禁止确认下发（计划保持挂起，仅供查看）
    QString blockedReason;
    if (_swarmManager->aiExecutionBlocked(&blockedReason)) {
        _appendChat(tr("系统"), tr("无法确认下发：%1").arg(blockedReason));
        emit agentFeedback(tr("L1 仅建议档位：禁止下发，请在面板切换自主等级"));
        return;
    }

    if (_pendingTask) {
        const QString taskName = _pendingTaskName;
        const QList<SwarmTaskController::Stage> stages = _pendingStages;
        const QStringList summary = planSummary();
        _pendingTask = false;
        _pendingStages.clear();
        emit planPendingChanged();
        emit planChanged();
        _appendChat(tr("系统"), tr("操作员已确认，多阶段任务启动。"));
        SwarmAuditLogger::instance()->logPlanConfirmed(summary);
        _swarmManager->taskController()->startTask(taskName, stages);
        return;
    }
    if (_pendingPlan.isEmpty()) {
        return;
    }
    const QList<SwarmAction> plan = _pendingPlan;
    const QStringList summary = planSummary();
    _pendingPlan.clear();
    emit planPendingChanged();
    emit planChanged();
    _appendChat(tr("系统"), tr("操作员已确认，计划开始执行。"));
    SwarmAuditLogger::instance()->logPlanConfirmed(summary);
    _swarmManager->executeActions(plan, QStringLiteral("ai_confirmed"));
}

void SwarmAIAgent::rejectPlan()
{
    if (_pendingPlan.isEmpty() && !_pendingTask) {
        return;
    }
    _pendingPlan.clear();
    _pendingTask = false;
    _pendingStages.clear();
    emit planPendingChanged();
    emit planChanged();
    _appendChat(tr("系统"), tr("操作员已放弃该计划。"));
    SwarmAuditLogger::instance()->logPlanRejected();
    emit agentFeedback(tr("已放弃 AI 行动计划"));
}

void SwarmAIAgent::clearHistory()
{
    _history.clear();
    _chatLog.clear();
    emit chatLogChanged();
    emit agentFeedback(tr("AI 对话历史已清空"));
}

void SwarmAIAgent::testConnection()
{
    if (_apiKey.trimmed().isEmpty()) {
        emit agentFeedback(tr("请先配置 API Key"));
        return;
    }
    QNetworkRequest request(QUrl(_apiBaseUrl + QStringLiteral("/models")));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + _apiKey.toUtf8());
    request.setTransferTimeout(15000);

    QNetworkReply *reply = _nam->get(request);
    (void) connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            emit agentFeedback(tr("连接成功：%1（模型 %2）").arg(_apiBaseUrl, _modelName));
        } else {
            emit agentFeedback(tr("连接失败：%1（HTTP %2）")
                    .arg(reply->errorString())
                    .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
        }
    });
    emit agentFeedback(tr("正在测试连接 %1 ...").arg(_apiBaseUrl));
}

void SwarmAIAgent::_setBusy(bool busy)
{
    if (_busy != busy) {
        _busy = busy;
        emit busyChanged();
    }
}

void SwarmAIAgent::_appendChat(const QString &who, const QString &text)
{
    _chatLog.append(QStringLiteral("[%1] %2").arg(who, text));
    emit chatLogChanged();
}
