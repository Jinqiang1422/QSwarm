#include "SwarmManager.h"

#include "FormationController.h"
#include "SwarmMissionPlanner.h"
#include "SwarmAIAgent.h"
#include "SwarmTaskController.h"
#include "SwarmTaskLibrary.h"
#include "SwarmAuditLogger.h"
#include "SwarmCOPModel.h"
#include "SwarmInputFilter.h"
#include "SwarmPerceptionBridge.h"
#include "SwarmRehearsalEstimator.h"
#include "SwarmSituationDigest.h"
#include "MissionManager.h"
#include "FactGroup.h"
#include "VehicleFactGroup.h"
#include "VehicleFactGroup.h"
#include "Fact.h"
#include "MultiVehicleManager.h"
#include "Vehicle.h"
#include "QmlObjectListModel.h"
#include "QGCApplication.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>

#include <algorithm>

Q_APPLICATION_STATIC(SwarmManager, _swarmManagerInstance);

SwarmManager::SwarmManager(QObject *parent)
    : QObject(parent)
    , _formation(new FormationController(this))
    , _missionPlanner(new SwarmMissionPlanner(this))
    , _aiAgent(new SwarmAIAgent(this))
    , _taskController(new SwarmTaskController(this))
    , _taskLibrary(new SwarmTaskLibrary(this))
    , _copModel(new SwarmCOPModel(this))
    , _perception(new SwarmPerceptionBridge(this))
{
    SwarmAuditLogger::instance();       // 尽早建立审计会话（WP1.1）
    _loadSettings();

    // P3：感知链路 → 航迹融合；2Hz 节拍驱动 prune/COP/事件检测/态势
    (void) connect(_perception, &SwarmPerceptionBridge::trackBatchReceived,
                   this, &SwarmManager::_onTrackBatch);
    _perceptionTimer = new QTimer(this);
    _perceptionTimer->setInterval(500);
    (void) connect(_perceptionTimer, &QTimer::timeout, this, &SwarmManager::_onPerceptionTick);
    _perceptionTimer->start();
}

SwarmManager::~SwarmManager() = default;

SwarmManager *SwarmManager::instance()
{
    return _swarmManagerInstance();
}

void SwarmManager::init()
{
    MultiVehicleManager *mvm = MultiVehicleManager::instance();
    (void) connect(mvm, &MultiVehicleManager::vehicleAdded,   this, &SwarmManager::_vehicleAdded);
    (void) connect(mvm, &MultiVehicleManager::vehicleRemoved, this, &SwarmManager::_vehicleRemoved);

    // 将指令反馈透传到 QGC 顶部应用消息条
    (void) connect(this, &SwarmManager::swarmCommandFeedback, qgcApp(), [](const QString &message) {
        qgcApp()->showAppMessage(message);
    });

    // P3：默认启动感知链路接收（QSettings 可关）
    if (QSettings().value(QStringLiteral("Swarm/perceptionAutoStart"), true).toBool()) {
        startPerception(QSettings().value(QStringLiteral("Swarm/perceptionPort"), 15600).toInt());
    }
    // 告警同时上应用消息条（critical 由 AIAgent 生成处置建议）
    (void) connect(this, &SwarmManager::swarmAlert, qgcApp(),
                   [](const QString &level, const QString &rule, const QString &message) {
        Q_UNUSED(rule);
        if (level != QStringLiteral("info")) {
            qgcApp()->showAppMessage(message);
        }
    });
}

QmlObjectListModel *SwarmManager::vehicles() const
{
    return MultiVehicleManager::instance()->vehicles();
}

QmlObjectListModel *SwarmManager::selectedVehicles() const
{
    return MultiVehicleManager::instance()->selectedVehicles();
}

int SwarmManager::vehicleCount() const
{
    return MultiVehicleManager::instance()->vehicles()->count();
}

void SwarmManager::setCommandIntervalMs(int intervalMs)
{
    if (_commandIntervalMs != intervalMs) {
        _commandIntervalMs = intervalMs;
        emit commandIntervalMsChanged();
    }
}

// ---------------------------------------------------------------------------
// 自主等级（WP1.3）与审计/过滤（WP1.1/WP1.2）接口
// ---------------------------------------------------------------------------

void SwarmManager::_loadSettings()
{
    QSettings settings;
    _autonomyLevel  = qBound(1, settings.value(QStringLiteral("Swarm/autonomyLevel"), 2).toInt(), 3);
    _l3AllowedTypes = settings.value(QStringLiteral("Swarm/l3AllowedTypes"),
                                     QVariant::fromValue(SwarmAutonomy::defaultL3AllowedTypes())).toStringList();
}

void SwarmManager::setAutonomyLevel(int level)
{
    level = qBound(1, level, 3);
    if (_autonomyLevel != level) {
        _autonomyLevel = level;
        QSettings().setValue(QStringLiteral("Swarm/autonomyLevel"), level);
        SwarmAuditLogger::instance()->logAutonomyChange(level);
        emit autonomyLevelChanged();
        emit swarmCommandFeedback(tr("自主等级已切换为 %1")
                                  .arg(SwarmAutonomy::levelName(static_cast<SwarmAutonomy::Level>(level))));
    }
}

void SwarmManager::setL3AllowedTypes(const QStringList &types)
{
    if (_l3AllowedTypes != types) {
        _l3AllowedTypes = types;
        QSettings().setValue(QStringLiteral("Swarm/l3AllowedTypes"), QVariant::fromValue(types));
        emit l3AllowedTypesChanged();
    }
}

QString SwarmManager::auditDir() const
{
    return SwarmAuditLogger::instance()->auditDir();
}

QString SwarmManager::autonomyLevelName() const
{
    return SwarmAutonomy::levelName(static_cast<SwarmAutonomy::Level>(_autonomyLevel));
}

QString SwarmManager::autonomyLevelDescription() const
{
    return SwarmAutonomy::levelDescription(static_cast<SwarmAutonomy::Level>(_autonomyLevel));
}

QString SwarmManager::auditSessionId() const
{
    return SwarmAuditLogger::instance()->sessionId();
}

int SwarmManager::auditEventCount() const
{
    return SwarmAuditLogger::instance()->eventCount();
}

QString SwarmManager::exportAudit()
{
    const QString path = SwarmAuditLogger::instance()->exportSession(auditDir() + QStringLiteral("/exports"));
    if (!path.isEmpty()) {
        emit swarmCommandFeedback(tr("审计日志已导出：%1").arg(path));
    } else {
        emit swarmCommandFeedback(tr("审计日志导出失败"));
    }
    return path;
}

QString SwarmManager::checkTextSafety(const QString &text) const
{
    const SwarmInputFilter::Verdict verdict = SwarmInputFilter::instance()->check(text, QStringLiteral("qml"));
    QJsonObject obj;
    obj[QStringLiteral("level")]   = verdict.level;
    obj[QStringLiteral("matched")] = QJsonArray::fromStringList(verdict.matched);
    obj[QStringLiteral("score")]   = verdict.score;
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

bool SwarmManager::aiExecutionBlocked(QString *reason) const
{
    const auto level = static_cast<SwarmAutonomy::Level>(_autonomyLevel);
    if (SwarmAutonomy::aiExecutionBlocked(level)) {
        if (reason) {
            *reason = tr("当前为 %1 档位：AI 计划仅展示，禁止下发执行").arg(SwarmAutonomy::levelName(level));
        }
        return true;
    }
    return false;
}

bool SwarmManager::needsConfirmation(const QList<SwarmAction> &actions, QString *reason) const
{
    return SwarmAutonomy::needsConfirmation(
                static_cast<SwarmAutonomy::Level>(_autonomyLevel), _l3AllowedTypes, actions, reason);
}

// ---------------------------------------------------------------------------
// 约束引擎（WP2.1）与解析式预演（WP2.7）
// ---------------------------------------------------------------------------

QList<SwarmTypes::VehicleState> SwarmManager::_vehicleStates() const
{
    QList<SwarmTypes::VehicleState> out;
    MultiVehicleManager *mvm = MultiVehicleManager::instance();
    QmlObjectListModel *model = (mvm->selectedVehicles()->count() > 0) ? mvm->selectedVehicles()
                                                                       : mvm->vehicles();
    for (int i = 0; i < model->count(); i++) {
        Vehicle *vehicle = qobject_cast<Vehicle *>(model->get(i));
        if (!vehicle) {
            continue;
        }
        SwarmTypes::VehicleState v;
        v.id     = vehicle->id();
        v.lat    = vehicle->coordinate().latitude();
        v.lon    = vehicle->coordinate().longitude();
        v.online = true;
        if (vehicle->batteries()->count() > 0) {
            FactGroup *bg = qobject_cast<FactGroup *>(vehicle->batteries()->get(0));
            if (bg && bg->getFact(QStringLiteral("percentRemaining"))) {
                v.batteryPct = bg->getFact(QStringLiteral("percentRemaining"))->rawValue().toDouble();
            }
        }
        VehicleFactGroup *vfg = qobject_cast<VehicleFactGroup *>(vehicle->vehicleFactGroup());
        v.altRelM = vfg ? vfg->altitudeRelative()->rawValue().toDouble() : 0.0;
        out.append(v);
    }
    return out;
}

SwarmConstraintChecker::Config SwarmManager::_constraintConfig() const
{
    const QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + QStringLiteral("/SwarmConfig/fences.json");
    QFile file(path);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return SwarmConstraintChecker::configFromJson(
                    QJsonDocument::fromJson(file.readAll()).object());
    }
    // 首次运行：写默认配置（全部不限制），供用户按需编辑
    const SwarmConstraintChecker::Config cfg;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QJsonObject doc = SwarmConstraintChecker::configToJson(cfg);
        doc[QStringLiteral("include")] = QJsonArray();
        doc[QStringLiteral("exclude")] = QJsonArray();
        out.write(QJsonDocument(doc).toJson(QJsonDocument::Indented));
    }
    return cfg;
}

QVariantList SwarmManager::checkConstraints(const QList<SwarmAction> &plan, double mapLat, double mapLon)
{
    SwarmConstraintChecker checker;
    const auto violations = checker.check(plan, _vehicleStates(), _constraintConfig(), mapLat, mapLon);

    int errors = 0;
    QVariantList out;
    for (const auto &v : violations) {
        if (v.severity == QStringLiteral("error")) {
            errors++;
        }
        out.append(QVariantMap{
            {QStringLiteral("severity"),   v.severity},
            {QStringLiteral("rule"),       v.rule},
            {QStringLiteral("message"),    v.message},
            {QStringLiteral("suggestion"), v.suggestion},
        });
    }
    SwarmAuditLogger::instance()->log(QStringLiteral("safety"), QStringLiteral("constraint_check"), {
        {QStringLiteral("errors"),   errors},
        {QStringLiteral("warnings"), violations.count() - errors},
        {QStringLiteral("actions"),  plan.count()}
    });
    return out;
}

QVariantMap SwarmManager::estimatePlan(const QList<SwarmAction> &plan, double mapLat, double mapLon)
{
    SwarmRehearsalEstimator estimator;
    const SwarmRehearsalEstimator::Estimate r = estimator.estimate(
                plan, _vehicleStates(), mapLat, mapLon);
    return {
        {QStringLiteral("duration_min"),       r.durationMin},
        {QStringLiteral("distance_km"),        r.distanceKm},
        {QStringLiteral("battery_margin_pct"), r.batteryMarginPct},
        {QStringLiteral("vehicles_used"),      r.vehiclesUsed},
        {QStringLiteral("notes"),              r.notes},
        {QStringLiteral("summary"),            SwarmRehearsalEstimator::summary(r)},
    };
}

QString SwarmManager::fenceSummary()
{
    const SwarmConstraintChecker::Config cfg = _constraintConfig();
    return tr("允许空域 %1 个｜禁飞区 %2 个｜高度上限 %3｜链路半径 %4 米")
            .arg(cfg.includePolygons.count())
            .arg(cfg.excludePolygons.count())
            .arg(cfg.maxAltitudeM > 0.0 ? QString::number(cfg.maxAltitudeM, 'f', 0) + tr(" 米") : tr("未设"))
            .arg(cfg.commRangeM, 0, 'f', 0);
}

// ---------------------------------------------------------------------------
// P3 感知融合与态势理解
// ---------------------------------------------------------------------------

bool SwarmManager::startPerception(int port)
{
    const bool ok = _perception->listen(quint16(port));
    if (ok) {
        // 威胁区图层 = 禁飞区多边形（fences.json exclude）
        const SwarmConstraintChecker::Config cfg = _constraintConfig();
        QVariantList zones;
        for (const auto &poly : cfg.excludePolygons) {
            QVariantList ring;
            for (const auto &pt : poly) {
                ring.append(QVariantList{pt.first, pt.second});
            }
            zones.append(ring);
        }
        _copModel->setThreatZones(zones);
        _eventDetector.setFences(cfg.includePolygons, cfg.excludePolygons);
        SwarmAuditLogger::instance()->log(QStringLiteral("exec"), QStringLiteral("perception_start"),
                                          {{QStringLiteral("port"), port}});
        emit swarmCommandFeedback(tr("感知链路已启动（端口 %1）").arg(port));
    } else {
        emit swarmCommandFeedback(tr("感知链路启动失败（端口 %1 被占用）").arg(port));
    }
    return ok;
}

void SwarmManager::stopPerception()
{
    _perception->stop();
    SwarmAuditLogger::instance()->log(QStringLiteral("exec"), QStringLiteral("perception_stop"), {});
}

void SwarmManager::_onTrackBatch(const QVariantList &tracks, qint64 timeUsec)
{
    const qint64 monoMs = QDateTime::currentMSecsSinceEpoch();
    for (const QVariant &v : tracks) {
        const QVariantMap m = v.toMap();
        SwarmTrackFusion::TrackReport r;
        r.trackId    = m.value(QStringLiteral("track_id")).toInt();
        r.sensorId   = m.value(QStringLiteral("sensor_id")).toInt();
        r.category   = m.value(QStringLiteral("category")).toInt();
        r.lat        = m.value(QStringLiteral("lat")).toDouble();
        r.lon        = m.value(QStringLiteral("lon")).toDouble();
        r.ve         = m.value(QStringLiteral("ve")).toDouble();
        r.vn         = m.value(QStringLiteral("vn")).toDouble();
        r.confidence = m.value(QStringLiteral("confidence"), 0.5).toDouble();
        r.source     = m.value(QStringLiteral("source")).toInt();
        r.timeUsec   = timeUsec;
        _fusion.ingest(r, monoMs);
    }
}

void SwarmManager::_onPerceptionTick()
{
    const qint64 monoMs = QDateTime::currentMSecsSinceEpoch();
    _fusion.prune(monoMs);

    const auto fused = _fusion.tracks();
    _copModel->updateFromFusion(fused, monoMs);

    // 覆盖热力：在线机体位置逐格累计
    const auto vehicles = _vehicleStates();
    for (const auto &v : vehicles) {
        if (v.online) {
            _copModel->ingestCoverage(v.lat, v.lon);
        }
    }

    // 事件检测：平台 + 目标
    _raiseAlerts(_eventDetector.checkVehicles(vehicles, monoMs));
    _raiseAlerts(_eventDetector.checkTracks(fused, monoMs));

    // 态势一行摘要（面板状态栏）
    const QJsonObject digest = SwarmSituationDigest::build(
                vehicles, fused,
                _taskController->stateText(), _taskController->running(),
                _alerts, monoMs);
    const QString oneLiner = SwarmSituationDigest::oneLiner(digest);
    if (oneLiner != _situationOneLiner) {
        _situationOneLiner = oneLiner;
        emit situationChanged();
    }
}

void SwarmManager::_raiseAlerts(const QList<SwarmEventDetector::Alert> &alerts)
{
    if (alerts.isEmpty()) {
        return;
    }
    for (const auto &a : alerts) {
        _alerts.prepend(a);
        _alertsVariant.prepend(QVariantMap{
            {QStringLiteral("level"),   a.level},
            {QStringLiteral("rule"),    a.rule},
            {QStringLiteral("message"), a.message},
            {QStringLiteral("ids"),     a.relatedIds},
            {QStringLiteral("text"),    SwarmEventDetector::alertToText(a)},
        });
        emit swarmAlert(a.level, a.rule, a.message);
        SwarmAuditLogger::instance()->log(QStringLiteral("safety"), QStringLiteral("alert"), {
            {QStringLiteral("level"),   a.level},
            {QStringLiteral("rule"),    a.rule},
            {QStringLiteral("message"), a.message},
        });
        // critical → LLM 处置建议（异步，结果回对话区）
        if (a.level == QStringLiteral("critical")) {
            _aiAgent->requestAdvice(a.message);
        }
    }
    while (_alerts.count() > 20) {
        _alerts.removeLast();
        _alertsVariant.removeLast();
    }
    emit alertsChanged();
}

QString SwarmManager::situationDigestJson() const
{
    const qint64 monoMs = QDateTime::currentMSecsSinceEpoch();
    const QJsonObject digest = SwarmSituationDigest::build(
                _vehicleStates(), _fusion.tracks(),
                _taskController->stateText(), _taskController->running(),
                _alerts, monoMs);
    return QString::fromUtf8(QJsonDocument(digest).toJson(QJsonDocument::Compact));
}

QList<Vehicle *> SwarmManager::_targetVehicles() const
{
    MultiVehicleManager *mvm = MultiVehicleManager::instance();
    // 优先作用于界面中选中的车辆，否则作用于全部在线车辆
    QmlObjectListModel *model = (mvm->selectedVehicles()->count() > 0) ? mvm->selectedVehicles()
                                                                       : mvm->vehicles();
    QList<Vehicle *> result;
    for (int i = 0; i < model->count(); i++) {
        Vehicle *vehicle = qobject_cast<Vehicle *>(model->get(i));
        if (vehicle) {
            result.append(vehicle);
        }
    }
    std::sort(result.begin(), result.end(), [](const Vehicle *a, const Vehicle *b) { return a->id() < b->id(); });
    return result;
}

void SwarmManager::_forEachVehicleStaggered(const QString &commandName, int staggerMs, const std::function<void(Vehicle *)> &action)
{
    const QList<Vehicle *> targets = _targetVehicles();
    if (targets.isEmpty()) {
        emit swarmCommandFeedback(tr("%1：当前没有在线无人机").arg(commandName));
        return;
    }

    int delayMs = 0;
    for (Vehicle *vehicle : targets) {
        QPointer<Vehicle> vehiclePtr(vehicle);
        QTimer::singleShot(delayMs, this, [vehiclePtr, action]() {
            if (vehiclePtr) {
                action(vehiclePtr.data());
            }
        });
        delayMs += staggerMs;
    }
    emit swarmCommandFeedback(tr("%1：已排队下发至 %2 架无人机").arg(commandName).arg(targets.count()));
}

void SwarmManager::armAll(bool arm)
{
    SwarmAuditLogger::instance()->logOperatorCommand(arm ? QStringLiteral("armAll") : QStringLiteral("disarmAll"));
    _forEachVehicleStaggered(arm ? tr("集群解锁") : tr("集群上锁"), _commandIntervalMs, [arm](Vehicle *vehicle) {
        vehicle->setArmed(arm, true /*showError*/);
    });
}

void SwarmManager::takeoffAll(double altitudeRelative)
{
    SwarmAuditLogger::instance()->logOperatorCommand(QStringLiteral("takeoffAll"),
                                                     {{QStringLiteral("altitude"), altitudeRelative}});
    _forEachVehicleStaggered(tr("集群起飞"), _takeoffStaggerMs, [this, altitudeRelative](Vehicle *vehicle) {
        if (vehicle->armed()) {
            vehicle->guidedModeTakeoff(altitudeRelative);
        } else {
            // 先解锁，等待解锁生效后再起飞
            vehicle->setArmed(true, true /*showError*/);
            QPointer<Vehicle> vehiclePtr(vehicle);
            QTimer::singleShot(1500, this, [vehiclePtr, altitudeRelative]() {
                if (vehiclePtr && vehiclePtr->armed()) {
                    vehiclePtr->guidedModeTakeoff(altitudeRelative);
                }
            });
        }
    });
}

void SwarmManager::landAll()
{
    SwarmAuditLogger::instance()->logOperatorCommand(QStringLiteral("landAll"));
    _forEachVehicleStaggered(tr("集群降落"), _commandIntervalMs, [](Vehicle *vehicle) {
        vehicle->guidedModeLand();
    });
}

void SwarmManager::rtlAll()
{
    SwarmAuditLogger::instance()->logOperatorCommand(QStringLiteral("rtlAll"));
    _forEachVehicleStaggered(tr("集群返航"), _commandIntervalMs, [](Vehicle *vehicle) {
        vehicle->guidedModeRTL(true /*smartRTL*/);
    });
}

void SwarmManager::pauseAll()
{
    SwarmAuditLogger::instance()->logOperatorCommand(QStringLiteral("pauseAll"));
    _forEachVehicleStaggered(tr("集群暂停"), 0 /*立即同时执行*/, [](Vehicle *vehicle) {
        vehicle->pauseVehicle();
    });
}

void SwarmManager::startMissionAll()
{
    SwarmAuditLogger::instance()->logOperatorCommand(QStringLiteral("startMissionAll"));
    _forEachVehicleStaggered(tr("集群开始任务"), _commandIntervalMs, [](Vehicle *vehicle) {
        vehicle->startMission();
    });
}

void SwarmManager::emergencyDisarmAll()
{
    SwarmAuditLogger::instance()->logOperatorCommand(QStringLiteral("emergencyDisarmAll"));
    _forEachVehicleStaggered(tr("紧急上锁"), 0, [](Vehicle *vehicle) {
        // MAV_CMD_COMPONENT_ARM_DISARM: param1=0(上锁), param2=21196(强制，含空中)
        vehicle->sendMavCommand(vehicle->defaultComponentId(), MAV_CMD_COMPONENT_ARM_DISARM, true /*showError*/,
                                0.0f /*disarm*/, 21196.0f /*force*/);
    });
}

// ---------------------------------------------------------------------------
// 统一动作管线：解析校验（大模型 / 任务状态机 / 人工界面共用）
// ---------------------------------------------------------------------------
QList<SwarmAction> SwarmManager::parseAndValidate(const QJsonArray &actionsJson,
                                                  const QGeoCoordinate &mapCenter,
                                                  QStringList &warnings) const
{
    static const QSet<QString> knownTypes = {
        QStringLiteral("set_selection"), QStringLiteral("arm"), QStringLiteral("takeoff"),
        QStringLiteral("land"), QStringLiteral("rtl"), QStringLiteral("pause"),
        QStringLiteral("start_mission"), QStringLiteral("disarm"), QStringLiteral("emergency_disarm"),
        QStringLiteral("start_formation"), QStringLiteral("stop_formation"),
        QStringLiteral("plan_coverage"), QStringLiteral("clear_missions")
    };

    QList<SwarmAction> plan;
    MultiVehicleManager *mvm = MultiVehicleManager::instance();

    QList<int> online;
    for (int i = 0; i < mvm->vehicles()->count(); i++) {
        Vehicle *vehicle = qobject_cast<Vehicle *>(mvm->vehicles()->get(i));
        if (vehicle) {
            online.append(vehicle->id());
        }
    }

    const int actionCount = qMin(actionsJson.count(), 10);      // 单次最多执行 10 个动作，防失控输出
    for (int i = 0; i < actionCount; i++) {
        const QJsonObject obj = actionsJson.at(i).toObject();
        const QString type = obj.value(QStringLiteral("type")).toString();
        if (!knownTypes.contains(type)) {
            warnings.append(tr("已忽略未知动作类型：%1").arg(type));
            continue;
        }

        SwarmAction action;
        action.type      = type;
        action.dangerous = SwarmAction::isDangerousType(type);

        if (type == QStringLiteral("set_selection")) {
            QList<int> ids;
            for (const QJsonValue &v : obj.value(QStringLiteral("vehicle_ids")).toArray()) {
                const int id = v.toInt(-1);
                if (online.contains(id)) {
                    ids.append(id);
                } else {
                    warnings.append(tr("机号 %1 不在线，已从选择中剔除").arg(id));
                }
            }
            if (ids.isEmpty()) {
                warnings.append(tr("set_selection 无有效机号，已忽略"));
                continue;
            }
            action.params[QStringLiteral("vehicle_ids")] = QVariant::fromValue(ids);
            QStringList idStrings;
            for (int id : ids) { idStrings.append(QString::number(id)); }
            action.description = tr("选中 %1 号机").arg(idStrings.join(QStringLiteral("、")));
        } else if (type == QStringLiteral("arm")) {
            const bool arm = obj.value(QStringLiteral("arm")).toBool();
            action.params[QStringLiteral("arm")] = arm;
            action.description = arm ? tr("集群解锁") : tr("集群上锁");
        } else if (type == QStringLiteral("takeoff")) {
            const double altRaw = obj.value(QStringLiteral("altitude")).toDouble(30.0);
            const double alt    = qBound(2.0, altRaw, 120.0);
            if (!qFuzzyCompare(alt, altRaw)) {
                warnings.append(tr("起飞高度 %1 米超限，已限制为 %2 米").arg(altRaw).arg(alt));
            }
            action.params[QStringLiteral("altitude")] = alt;
            action.description = tr("集群起飞至 %1 米").arg(alt);
        } else if (type == QStringLiteral("land")) {
            action.description = tr("集群原地降落");
        } else if (type == QStringLiteral("rtl")) {
            action.description = tr("集群返航");
        } else if (type == QStringLiteral("pause")) {
            action.description = tr("集群暂停悬停");
        } else if (type == QStringLiteral("start_mission")) {
            action.description = tr("集群开始任务");
        } else if (type == QStringLiteral("disarm")) {
            action.description = tr("集群上锁");
        } else if (type == QStringLiteral("emergency_disarm")) {
            action.description = tr("【危险】集群强制上锁");
        } else if (type == QStringLiteral("start_formation")) {
            const int leaderId = obj.value(QStringLiteral("leader_id")).toInt(-1);
            if (!online.contains(leaderId)) {
                warnings.append(tr("长机 %1 不在线，编队动作已忽略").arg(leaderId));
                continue;
            }
            static const QMap<QString, QString> typeNameMap = {
                {QStringLiteral("wedge"),  tr("楔形")}, {QStringLiteral("column"), tr("纵队")},
                {QStringLiteral("line"),   tr("横队")}, {QStringLiteral("circle"), tr("环形")}
            };
            QString formationName = obj.value(QStringLiteral("formation")).toString(QStringLiteral("wedge"));
            if (!typeNameMap.contains(formationName)) {
                warnings.append(tr("未知队形 %1，已按楔形处理").arg(formationName));
                formationName = QStringLiteral("wedge");
            }
            const double spacing = qBound(2.0, obj.value(QStringLiteral("spacing")).toDouble(10.0), 500.0);
            const double vstep   = qBound(0.0, obj.value(QStringLiteral("vertical_step")).toDouble(3.0), 50.0);
            action.params[QStringLiteral("leader_id")]     = leaderId;
            action.params[QStringLiteral("formation")]     = formationName;
            action.params[QStringLiteral("spacing")]       = spacing;
            action.params[QStringLiteral("vertical_step")] = vstep;
            action.description = tr("以 %1 号为长机开始%2编队（间距 %3 米，分层 %4 米）")
                    .arg(leaderId).arg(typeNameMap.value(formationName)).arg(spacing).arg(vstep);
        } else if (type == QStringLiteral("stop_formation")) {
            action.description = tr("停止编队");
        } else if (type == QStringLiteral("plan_coverage")) {
            double centerLat = mapCenter.latitude(), centerLon = mapCenter.longitude();
            const QJsonValue centerVal = obj.value(QStringLiteral("center"));
            if (centerVal.isArray() && centerVal.toArray().count() >= 2) {
                centerLat = centerVal.toArray().at(0).toDouble();
                centerLon = centerVal.toArray().at(1).toDouble();
            }
            const QGeoCoordinate center(centerLat, centerLon);
            if (!center.isValid()) {
                warnings.append(tr("区域中心坐标无效，任务规划动作已忽略"));
                continue;
            }
            const double length = qBound(10.0, obj.value(QStringLiteral("length")).toDouble(200.0), 100000.0);
            const double width  = qBound(10.0, obj.value(QStringLiteral("width")).toDouble(120.0), 100000.0);
            const double angle  = qBound(0.0, obj.value(QStringLiteral("angle")).toDouble(0.0), 360.0);
            const double spacing= qBound(2.0, obj.value(QStringLiteral("spacing")).toDouble(20.0), 1000.0);
            const double alt    = qBound(3.0, obj.value(QStringLiteral("altitude")).toDouble(30.0), 120.0);
            // WP2.4：条带分配方式（sequential 默认 / auction 拍卖法）
            const QString assignRaw = obj.value(QStringLiteral("assign")).toString();
            QString assignMode = QStringLiteral("sequential");
            if (assignRaw == QStringLiteral("auction") || assignRaw == QStringLiteral("sequential")) {
                assignMode = assignRaw;
            } else if (!assignRaw.isEmpty()) {
                warnings.append(tr("未知分配方式「%1」，已回退为顺序分配").arg(assignRaw));
            }
            action.params[QStringLiteral("lat")]     = centerLat;
            action.params[QStringLiteral("lon")]     = centerLon;
            action.params[QStringLiteral("length")]  = length;
            action.params[QStringLiteral("width")]   = width;
            action.params[QStringLiteral("angle")]   = angle;
            action.params[QStringLiteral("spacing")] = spacing;
            action.params[QStringLiteral("altitude")]= alt;
            action.params[QStringLiteral("assign")]  = assignMode;
            action.description = tr("区域覆盖任务规划：%1×%2 米，角度 %3°，行距 %4 米，高度 %5 米%6")
                    .arg(length).arg(width).arg(angle).arg(spacing).arg(alt)
                    .arg(assignMode == QStringLiteral("auction") ? tr("（拍卖分配）") : QString());
        } else if (type == QStringLiteral("clear_missions")) {
            action.description = tr("清空各机任务");
        }

        plan.append(action);
    }
    return plan;
}

// ---------------------------------------------------------------------------
// 统一动作管线：执行
// ---------------------------------------------------------------------------
void SwarmManager::executeActions(const QList<SwarmAction> &actions, const QString &origin)
{
    MultiVehicleManager *mvm = MultiVehicleManager::instance();

    SwarmAuditLogger::instance()->log(QStringLiteral("exec"), QStringLiteral("action_sequence"), {
        {QStringLiteral("origin"), origin},
        {QStringLiteral("count"),  actions.count()}
    });

    for (const SwarmAction &action : actions) {
        const QString &type = action.type;
        const QVariantMap &p = action.params;

        SwarmAuditLogger::instance()->logActionDispatch(type, p, action.description);

        if (type == QStringLiteral("set_selection")) {
            mvm->deselectAllVehicles();
            for (const QVariant &v : p.value(QStringLiteral("vehicle_ids")).toList()) {
                mvm->selectVehicle(v.toInt());
            }
        } else if (type == QStringLiteral("arm")) {
            armAll(p.value(QStringLiteral("arm")).toBool());
        } else if (type == QStringLiteral("takeoff")) {
            takeoffAll(p.value(QStringLiteral("altitude")).toDouble());
        } else if (type == QStringLiteral("land")) {
            landAll();
        } else if (type == QStringLiteral("rtl")) {
            rtlAll();
        } else if (type == QStringLiteral("pause")) {
            pauseAll();
        } else if (type == QStringLiteral("start_mission")) {
            startMissionAll();
        } else if (type == QStringLiteral("disarm")) {
            armAll(false);
        } else if (type == QStringLiteral("emergency_disarm")) {
            emergencyDisarmAll();
        } else if (type == QStringLiteral("start_formation")) {
            const QString name = p.value(QStringLiteral("formation")).toString();
            FormationController::FormationType formationType = FormationController::FormationWedge;
            if (name == QStringLiteral("column"))      formationType = FormationController::FormationColumn;
            else if (name == QStringLiteral("line"))   formationType = FormationController::FormationLine;
            else if (name == QStringLiteral("circle")) formationType = FormationController::FormationCircle;
            _formation->setFormationType(formationType);
            _formation->setSpacingMeters(p.value(QStringLiteral("spacing")).toDouble());
            _formation->setVerticalStepMeters(p.value(QStringLiteral("vertical_step")).toDouble());
            _formation->start(p.value(QStringLiteral("leader_id")).toInt());
        } else if (type == QStringLiteral("stop_formation")) {
            _formation->stop();
        } else if (type == QStringLiteral("plan_coverage")) {
            _missionPlanner->setSurveyAltitude(p.value(QStringLiteral("altitude")).toDouble());
            _missionPlanner->setSurveySpacing(p.value(QStringLiteral("spacing")).toDouble());
            _missionPlanner->planAndUploadCoverage(
                QGeoCoordinate(p.value(QStringLiteral("lat")).toDouble(),
                               p.value(QStringLiteral("lon")).toDouble()),
                p.value(QStringLiteral("length")).toDouble(),
                p.value(QStringLiteral("width")).toDouble(),
                p.value(QStringLiteral("angle")).toDouble(),
                p.value(QStringLiteral("assign"), QStringLiteral("sequential")).toString());
        } else if (type == QStringLiteral("clear_missions")) {
            _missionPlanner->clearAllMissions();
        }
    }
    emit swarmCommandFeedback(tr("动作序列已执行（%1 项）").arg(actions.count()));
}

void SwarmManager::_vehicleAdded(Vehicle *vehicle)
{
    Q_UNUSED(vehicle);
    emit vehicleCountChanged();
}

void SwarmManager::_vehicleRemoved(Vehicle *vehicle)
{
    Q_UNUSED(vehicle);
    emit vehicleCountChanged();
}
