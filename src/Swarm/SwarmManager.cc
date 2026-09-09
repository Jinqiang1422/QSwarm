#include "SwarmManager.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QScopedValueRollback>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <algorithm>
#include <cmath>

#include "Fact.h"
#include "FactGroup.h"
#include "FirmwarePlugin.h"
#include "FormationController.h"
#include "MissionManager.h"
#include "MultiVehicleManager.h"
#include "QGCApplication.h"
#include "QmlObjectListModel.h"
#include "SwarmAIAgent.h"
#include "SwarmAuditLogger.h"
#include "SwarmCOPModel.h"
#include "SwarmClock.h"
#include "SwarmConstraintStore.h"
#include "SwarmInputFilter.h"
#include "SwarmMissionPlanner.h"
#include "SwarmPerceptionBridge.h"
#include "SwarmRehearsalEstimator.h"
#include "SwarmSituationDigest.h"
#include "SwarmTactics.h"
#include "SwarmTaskController.h"
#include "SwarmTaskLibrary.h"
#include "Vehicle.h"
#include "VehicleFactGroup.h"
#include "VehicleLinkManager.h"

#ifdef QGC_UNITTEST_BUILD
#include "SwarmAcceptanceServer.h"
#endif

namespace {
QString validateActionInput(const QJsonObject& obj)
{
    const QString type = obj.value(QStringLiteral("type")).toString();
    QMap<QString, QPair<double, double>> numbers;
    QStringList required;
    QStringList extra;
    if (type == QStringLiteral("takeoff")) {
        numbers = {{"altitude", {2, 120}}};
        required = {"altitude"};
    } else if (type == QStringLiteral("arm")) {
        extra = {"arm"};
        if (!obj.value(QStringLiteral("arm")).isBool())
            return QStringLiteral("arm 必须为布尔值");
    } else if (type == QStringLiteral("set_selection")) {
        extra = {"vehicle_ids"};
        const auto ids = obj.value(QStringLiteral("vehicle_ids"));
        if (!ids.isArray() || ids.toArray().isEmpty())
            return QStringLiteral("目标机号不能为空");
        for (const auto& id : ids.toArray()) {
            if (!id.isDouble() || id.toDouble() != id.toInt(-1) || id.toInt(-1) < 1 || id.toInt() > 255)
                return QStringLiteral("机号必须为 1–255 的整数");
        }
    } else if (type == QStringLiteral("start_formation")) {
        numbers = {{"leader_id", {1, 255}}, {"spacing", {2, 500}}, {"vertical_step", {0, 50}}};
        required = {"leader_id", "formation"};
        extra = {"formation"};
        if (!QStringList{"wedge", "column", "line", "circle"}.contains(obj.value("formation").toString()) ||
            obj.value("leader_id").toDouble() != obj.value("leader_id").toInt(-1))
            return QStringLiteral("队形或长机机号无效");
    } else if (type == QStringLiteral("plan_coverage")) {
        numbers = {{"length", {10, 100000}},   {"width", {10, 100000}}, {"angle", {0, 360}},
                   {"spacing", {2, 1000}},     {"altitude", {3, 120}},  {"center_lat", {-90, 90}},
                   {"center_lon", {-180, 180}}};
        required = {"length", "width"};
        extra = {"center", "assign"};
        if (obj.contains("assign") &&
            !QStringList{"sequential", "auction", "cbba"}.contains(obj.value("assign").toString()))
            return QStringLiteral("分配方式无效");
        if (obj.contains("center_lat") != obj.contains("center_lon"))
            return QStringLiteral("区域中心经纬度必须成对提供");
        if (obj.contains("center") && obj.contains("center_lat"))
            return QStringLiteral("区域中心只能使用一种坐标格式");
        if (obj.contains("center") && obj.value("center").toString() != QStringLiteral("map_center")) {
            const auto center = obj.value("center").toArray();
            if (center.size() != 2 || !center[0].isDouble() || !center[1].isDouble() ||
                !QGeoCoordinate(center[0].toDouble(), center[1].toDouble()).isValid())
                return QStringLiteral("区域中心坐标无效");
        }
    } else if (type == QStringLiteral("plan_tactic")) {
        numbers = {{"target_lat", {-90, 90}},  {"target_lon", {-180, 180}},  {"radius", {10, 5000}},
                   {"target_speed", {0, 340}}, {"target_heading", {0, 360}}, {"intercept_speed", {1, 100}}};
        extra = {"scenario", "polygon"};
        const QString scenario = obj.value("scenario").toString();
        if (!QStringList{"track", "encircle", "intercept", "patrol"}.contains(scenario))
            return QStringLiteral("战术类型无效");
        if (scenario == QStringLiteral("patrol")) {
            const auto polygon = obj.value("polygon").toArray();
            if (polygon.size() < 3)
                return QStringLiteral("巡逻多边形至少需要三个顶点");
            for (const auto& point : polygon) {
                const auto pair = point.toArray();
                if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble() ||
                    !QGeoCoordinate(pair[0].toDouble(), pair[1].toDouble()).isValid())
                    return QStringLiteral("巡逻顶点无效");
            }
        } else {
            required = {"target_lat", "target_lon"};
        }
    }
    for (const auto& key : required) {
        if (!obj.contains(key))
            return QStringLiteral("缺少必填参数：%1").arg(key);
    }
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        if (numbers.contains(it.key())) {
            const auto range = numbers.value(it.key());
            if (!it.value().isDouble() || !std::isfinite(it.value().toDouble()) ||
                it.value().toDouble() < range.first || it.value().toDouble() > range.second)
                return QStringLiteral("参数类型或范围无效：%1").arg(it.key());
        } else if (it.key() != QStringLiteral("type") && !extra.contains(it.key())) {
            return QStringLiteral("未知参数：%1").arg(it.key());
        }
    }
    return {};
}

bool originHasOperatorAuthorization(const QString& origin, const SwarmTaskController* taskController)
{
    if (origin == QStringLiteral("operator") || origin == QStringLiteral("ai_confirmed") ||
        origin == QStringLiteral("template")) {
        return true;
    }
    // 多阶段任务的两个现有启动入口均是操作员确认 AI 计划或直接点击模板。
    // 只认可首阶段，后续自动推进仍按当前模式守门。
    return origin == QStringLiteral("task") && taskController && taskController->currentStageIndex() == 0;
}
}  // namespace

struct SwarmManager::Execution
{
    quint64 id = 0;
    QList<SwarmAction> actions;
    int index = 0;
    QString origin;
    ExecutionApproval approval = ExecutionApproval::Automatic;
    QPointer<QObject> context;
    QHash<int, QPointer<Vehicle>> targets;
    QHash<int, quint64> generations;
    QHash<int, SwarmMissionSnapshot> plannedMissions;
    QHash<int, SwarmMissionSnapshot> validatedMissions;
    QHash<int, quint64> contentRevisions;
    quint64 plannerBatch = 0;
    QSet<int> withdrawn;
    bool finished = false;
    quint64 policyRevision = 0;
    std::optional<SwarmAction> activeAction;
    QString failureCode;
    QString failureReason;
};

Q_APPLICATION_STATIC(SwarmManager, _swarmManagerInstance);

SwarmManager::SwarmManager(QObject* parent)
    : QObject(parent),
      _formation(new FormationController(this)),
      _missionPlanner(new SwarmMissionPlanner(this)),
      _aiAgent(new SwarmAIAgent(this)),
      _taskController(new SwarmTaskController(this)),
      _taskLibrary(new SwarmTaskLibrary(this)),
      _copModel(new SwarmCOPModel(this)),
      _perception(new SwarmPerceptionBridge(this))
{
    SwarmAuditLogger::instance();  // 尽早建立审计会话（WP1.1）
    _loadSettings();
    _constraintStore = new SwarmConstraintStore(
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/SwarmConfig/fences.json"),
        this);
    connect(_constraintStore, &SwarmConstraintStore::changed, this, &SwarmManager::_syncConstraintConsumers);
    _syncConstraintConsumers();

    _formation->setVehicleAutomationAllowedCallback(
        [this](int vehicleId) { return _formationAutomationAllowed(vehicleId); });
    _formation->setTargetAllowedCallback([this](int id, const QGeoCoordinate& point) {
        QPointer<Vehicle> vehicle = MultiVehicleManager::instance()->getVehicleById(id);
        if (!vehicle || !std::isfinite(point.altitude()) || !vehicle->coordinate().isValid())
            return false;
        const auto config = _constraintConfig();
        if (!vehicle || !config.error.isEmpty())
            return false;
        if (!config.includePolygons.isEmpty() &&
            std::none_of(config.includePolygons.begin(), config.includePolygons.end(), [&point](const auto& polygon) {
                return SwarmTypes::pointInPolygon(point.latitude(), point.longitude(), polygon);
            }))
            return false;
        for (const auto& polygon : config.excludePolygons)
            if (SwarmTypes::pointInPolygon(point.latitude(), point.longitude(), polygon))
                return false;
        if (config.maxAltitudeM > 0 &&
            (!vehicle->homePosition().isValid() || !std::isfinite(vehicle->homePosition().altitude()) ||
             point.altitude() - vehicle->homePosition().altitude() > config.maxAltitudeM))
            return false;
        for (int otherId : _formationTargetIds) {
            Vehicle* other = MultiVehicleManager::instance()->getVehicleById(otherId);
            if (!other || otherId == id || !other->coordinate().isValid())
                continue;
            const double dz = point.altitude() - other->altitudeAMSL()->rawValue().toDouble();
            if (std::isfinite(dz) && std::hypot(point.distanceTo(other->coordinate()), dz) < config.minSeparationM)
                return false;
        }
        return true;
    });
    _missionPlanner->setVehicleAutomationAllowedCallback([this](int vehicleId) {
        return !_automationExcludedIds.contains(vehicleId) &&
               (!_dispatchTargetIds || _dispatchTargetIds->contains(vehicleId)) &&
               _lease.canAutomate(vehicleId, SwarmClock::nowMs());
    });

    // P3：感知链路 → 航迹融合；2Hz 节拍驱动 prune/COP/事件检测/态势
    (void) connect(_perception, &SwarmPerceptionBridge::trackBatchReceived, this, &SwarmManager::_onTrackBatch);
    _perceptionTimer = new QTimer(this);
    _perceptionTimer->setInterval(500);
    (void) connect(_perceptionTimer, &QTimer::timeout, this, &SwarmManager::_onPerceptionTick);
    _perceptionTimer->start();
}

SwarmManager::~SwarmManager() = default;

SwarmManager* SwarmManager::instance()
{
    return _swarmManagerInstance();
}

void SwarmManager::init()
{
    MultiVehicleManager* mvm = MultiVehicleManager::instance();
    (void) connect(mvm, &MultiVehicleManager::vehicleAdded, this, &SwarmManager::_vehicleAdded);
    (void) connect(mvm, &MultiVehicleManager::vehicleRemoved, this, &SwarmManager::_vehicleRemoved);

    // 将指令反馈透传到 QGC 顶部应用消息条
    (void) connect(this, &SwarmManager::swarmCommandFeedback, qgcApp(),
                   [](const QString& message) { qgcApp()->showAppMessage(message); });

    // P3：默认启动感知链路接收（QSettings 可关）
    if (QSettings().value(QStringLiteral("Swarm/perceptionAutoStart"), true).toBool()) {
        startPerception(QSettings().value(QStringLiteral("Swarm/perceptionPort"), 15600).toInt());
    }
    // 告警同时上应用消息条（critical 由 AIAgent 生成处置建议）
    (void) connect(this, &SwarmManager::swarmAlert, qgcApp(),
                   [](const QString& level, const QString& rule, const QString& message) {
                       Q_UNUSED(rule);
                       if (level != QStringLiteral("info")) {
                           qgcApp()->showAppMessage(message);
                       }
                   });

#ifdef QGC_UNITTEST_BUILD
    _acceptanceServer = new SwarmAcceptanceServer(this);
    if (_acceptanceServer->startFromEnvironment()) {
        emit swarmCommandFeedback(tr("集群验收接口已在本机回环地址启动"));
    }
#endif
}

void SwarmManager::shutdown()
{
    if (_shutdownComplete) {
        return;
    }
    _shutdownComplete = true;
    const auto executions = _executions.values();
    for (const auto& execution : executions)
        _finishExecution(execution, false, tr("应用关闭"));
    _missionPlanner->cancelTargets(_commandGenerations.keys());

    _formation->stop();
    _taskController->shutdown();
    _aiAgent->shutdown();
    _perceptionTimer->stop();
    _perception->stop();
#ifdef QGC_UNITTEST_BUILD
    if (_acceptanceServer) {
        _acceptanceServer->stop();
    }
#endif
}

QmlObjectListModel* SwarmManager::vehicles() const
{
    return MultiVehicleManager::instance()->vehicles();
}

QmlObjectListModel* SwarmManager::selectedVehicles() const
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
    _autonomyLevel = qBound(1, settings.value(QStringLiteral("Swarm/autonomyLevel"), 2).toInt(), 3);
    _l3AllowedTypes =
        settings
            .value(QStringLiteral("Swarm/l3AllowedTypes"), QVariant::fromValue(SwarmAutonomy::defaultL3AllowedTypes()))
            .toStringList();
}

void SwarmManager::setAutonomyLevel(int level)
{
    level = qBound(1, level, 3);
    if (_autonomyLevel != level) {
        _autonomyLevel = level;
        ++_policyRevision;
        _enforcePolicy();
        QSettings().setValue(QStringLiteral("Swarm/autonomyLevel"), level);
        SwarmAuditLogger::instance()->logAutonomyChange(level);
        emit autonomyLevelChanged();
        emit swarmCommandFeedback(
            tr("自主等级已切换为 %1").arg(SwarmAutonomy::levelName(static_cast<SwarmAutonomy::Level>(level))));
    }
}

void SwarmManager::setL3AllowedTypes(const QStringList& types)
{
    if (_l3AllowedTypes != types) {
        _l3AllowedTypes = types;
        ++_policyRevision;
        _enforcePolicy();
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

QString SwarmManager::checkTextSafety(const QString& text) const
{
    const SwarmInputFilter::Verdict verdict = SwarmInputFilter::instance()->check(text, QStringLiteral("qml"));
    QJsonObject obj;
    obj[QStringLiteral("level")] = verdict.level;
    obj[QStringLiteral("matched")] = QJsonArray::fromStringList(verdict.matched);
    obj[QStringLiteral("score")] = verdict.score;
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

#ifdef QGC_UNITTEST_BUILD
QString SwarmManager::controlLeaseState(int vehicleId) const
{
    switch (_lease.stateOf(vehicleId)) {
        case SwarmControlLease::OBSERVE_ONLY:
            return QStringLiteral("observe_only");
        case SwarmControlLease::LEASE_REQUESTED:
            return QStringLiteral("lease_requested");
        case SwarmControlLease::LEASED:
            return QStringLiteral("leased");
        case SwarmControlLease::RELEASED:
            return QStringLiteral("released");
        case SwarmControlLease::MANUAL_OVERRIDE:
            return QStringLiteral("manual_override");
    }
    return QStringLiteral("unknown");
}

bool SwarmManager::automationAllowed(int vehicleId) const
{
    return _lease.canAutomate(vehicleId, SwarmClock::nowMs());
}
#endif

bool SwarmManager::aiExecutionBlocked(QString* reason) const
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

bool SwarmManager::needsConfirmation(const QList<SwarmAction>& actions, QString* reason) const
{
    return SwarmAutonomy::needsConfirmation(static_cast<SwarmAutonomy::Level>(_autonomyLevel), _l3AllowedTypes, actions,
                                            reason);
}

// ---------------------------------------------------------------------------
// 约束引擎（WP2.1）与解析式预演（WP2.7）
// ---------------------------------------------------------------------------

QList<SwarmTypes::VehicleState> SwarmManager::_vehicleStates(const std::optional<QList<int>>& ids) const
{
    QList<SwarmTypes::VehicleState> out;
    MultiVehicleManager* mvm = MultiVehicleManager::instance();
    QmlObjectListModel* model =
        ids ? mvm->vehicles() : ((mvm->selectedVehicles()->count() > 0) ? mvm->selectedVehicles() : mvm->vehicles());
    for (int i = 0; i < model->count(); i++) {
        Vehicle* vehicle = qobject_cast<Vehicle*>(model->get(i));
        if (vehicle && ids && !ids->contains(vehicle->id()))
            continue;
        if (!vehicle) {
            continue;
        }
        SwarmTypes::VehicleState v;
        v.id = vehicle->id();
        v.lat = vehicle->coordinate().latitude();
        v.lon = vehicle->coordinate().longitude();
        v.online = !vehicle->vehicleLinkManager()->communicationLost();
        const QGeoCoordinate home = vehicle->homePosition().isValid() ? vehicle->homePosition() : vehicle->coordinate();
        v.homeLat = home.latitude();
        v.homeLon = home.longitude();
        if (vehicle->batteries()->count() > 0) {
            FactGroup* bg = qobject_cast<FactGroup*>(vehicle->batteries()->get(0));
            if (bg && bg->getFact(QStringLiteral("percentRemaining"))) {
                v.batteryPct = bg->getFact(QStringLiteral("percentRemaining"))->rawValue().toDouble();
            }
        }
        VehicleFactGroup* vfg = qobject_cast<VehicleFactGroup*>(vehicle->vehicleFactGroup());
        v.altRelM = vfg ? vfg->altitudeRelative()->rawValue().toDouble() : 0.0;
        out.append(v);
    }
    return out;
}

SwarmConstraintChecker::Config SwarmManager::_constraintConfig() const
{
    return _constraintStore->snapshot();
}

bool SwarmManager::constraintsValid() const
{
    return _constraintStore->error().isEmpty();
}

QString SwarmManager::constraintsError() const
{
    return _constraintStore->error();
}

quint64 SwarmManager::constraintRevision() const
{
    return _constraintStore->revision();
}

void SwarmManager::_syncConstraintConsumers()
{
    const auto config = _constraintStore->lastValid();
    QVariantList zones;
    for (const auto& polygon : config.excludePolygons) {
        QVariantList ring;
        for (const auto& point : polygon)
            ring.append(QVariantList{point.first, point.second});
        zones.append(ring);
    }
    _copModel->setThreatZones(zones);
    _eventDetector.setFences(config.includePolygons, config.excludePolygons);
    emit constraintsChanged();
}

QList<SwarmConstraintChecker::Violation> SwarmManager::_planViolations(const QList<SwarmAction>& actions, double mapLat,
                                                                       double mapLon) const
{
    SwarmConstraintChecker checker;
    const auto config = _constraintConfig();
    QList<SwarmConstraintChecker::Violation> result;
    for (const auto& action : actions) {
        const auto states = _vehicleStates(action.targetIds);
        const bool requiresTargets = action.type != QStringLiteral("stop_formation");
        if (requiresTargets &&
            (states.isEmpty() || states.size() != action.targetIds.size() ||
             std::any_of(states.cbegin(), states.cend(), [](const auto& state) { return !state.online; }))) {
            result.append({QStringLiteral("error"), QStringLiteral("fleet"), tr("目标机缺失或离线"),
                           tr("重新选择目标并生成计划")});
        }
        // Use the same physical reference during preview, confirmation and stage execution.
        const double referenceLat = states.isEmpty() ? mapLat : states.first().homeLat;
        const double referenceLon = states.isEmpty() ? mapLon : states.first().homeLon;
        result.append(checker.check({action}, states, config, referenceLat, referenceLon));
    }
    result.append(checker.checkDeconfliction(actions, config, mapLat, mapLon));
    return result;
}

QVariantList SwarmManager::checkConstraints(const QList<SwarmAction>& plan, double mapLat, double mapLon)
{
    const auto violations = _planViolations(plan, mapLat, mapLon);

    int errors = 0;
    QVariantList out;
    for (const auto& v : violations) {
        if (v.severity == QStringLiteral("error")) {
            errors++;
        }
        out.append(QVariantMap{
            {QStringLiteral("severity"), v.severity},
            {QStringLiteral("rule"), v.rule},
            {QStringLiteral("message"), v.message},
            {QStringLiteral("suggestion"), v.suggestion},
        });
    }
    SwarmAuditLogger::instance()->log(QStringLiteral("safety"), QStringLiteral("constraint_check"),
                                      {{QStringLiteral("errors"), errors},
                                       {QStringLiteral("warnings"), violations.count() - errors},
                                       {QStringLiteral("actions"), plan.count()}});
    return out;
}

bool SwarmManager::planHasConstraintError(const QList<SwarmAction>& actions) const
{
    return SwarmConstraintChecker::hasError(_planViolations(actions, 0.0, 0.0));
}

QVariantMap SwarmManager::estimatePlan(const QList<SwarmAction>& plan, double mapLat, double mapLon)
{
    SwarmRehearsalEstimator estimator;
    QList<int> ids;
    for (const auto& action : plan) {
        for (int id : action.targetIds)
            if (!ids.contains(id))
                ids.append(id);
    }
    const SwarmRehearsalEstimator::Estimate r = estimator.estimate(plan, _vehicleStates(ids), mapLat, mapLon);
    return {
        {QStringLiteral("duration_min"), r.durationMin},
        {QStringLiteral("distance_km"), r.distanceKm},
        {QStringLiteral("battery_margin_pct"), r.batteryMarginPct},
        {QStringLiteral("vehicles_used"), r.vehiclesUsed},
        {QStringLiteral("notes"), r.notes},
        {QStringLiteral("summary"), SwarmRehearsalEstimator::summary(r)},
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
    const QSettings settings;
    const bool ok = _perception->listen(
        quint16(port),
        settings.value(QStringLiteral("Swarm/perceptionBindAddress"), QStringLiteral("127.0.0.1")).toString(),
        settings.value(QStringLiteral("Swarm/perceptionAllowedSources")).toStringList());
    if (ok) {
        _constraintStore->refresh();
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
    ++_perceptionGeneration;
    _pendingReports.clear();
    _drainScheduled = false;
    _perception->stop();
    SwarmAuditLogger::instance()->log(QStringLiteral("exec"), QStringLiteral("perception_stop"), {});
}

void SwarmManager::_onTrackBatch(const QVariantList& tracks, qint64 timeUsec)
{
    const qint64 monoMs = SwarmClock::nowMs();
    for (const QVariant& v : tracks) {
        const QVariantMap m = v.toMap();
        SwarmTrackFusion::TrackReport r;
        r.trackId = m.value(QStringLiteral("track_id")).toInt();
        r.sensorId = m.value(QStringLiteral("sensor_id")).toInt();
        r.category = m.value(QStringLiteral("category")).toInt();
        bool latitudeValid = false, longitudeValid = false;
        r.lat = m.value(QStringLiteral("lat")).toDouble(&latitudeValid);
        r.lon = m.value(QStringLiteral("lon")).toDouble(&longitudeValid);
        if (!latitudeValid || !longitudeValid || !SwarmTypes::isFiniteLatLon(r.lat, r.lon)) {
            ++_rejectedCoordinates;
            continue;
        }
        r.ve = m.value(QStringLiteral("ve")).toDouble();
        r.vn = m.value(QStringLiteral("vn")).toDouble();
        r.confidence = m.value(QStringLiteral("confidence"), 0.5).toDouble();
        r.source = m.value(QStringLiteral("source")).toInt();
        r.timeUsec = m.value(QStringLiteral("time_usec"), timeUsec).toLongLong();
        r.streamId = m.value(QStringLiteral("stream_id")).toString();
        if (_pendingReports.size() >= 8192) {
            _pendingReports.pop_front();
            ++_droppedReports;
        }
        _pendingReports.push_back({r, monoMs});
    }
    if (!_drainScheduled)
        _drainTrackReports();
}

void SwarmManager::_drainTrackReports()
{
    _drainScheduled = false;
    if (_shutdownComplete)
        return;
    QElapsedTimer elapsed;
    elapsed.start();
    for (int count = 0; !_pendingReports.empty() && count < 128 && elapsed.nsecsElapsed() < 2000000; ++count) {
        const auto pending = _pendingReports.front();
        _pendingReports.pop_front();
        _fusion.ingest(pending.report, pending.receivedMs);
    }
    if (!_pendingReports.empty()) {
        _drainScheduled = true;
        QTimer::singleShot(0, this, [this, generation = _perceptionGeneration] {
            if (generation == _perceptionGeneration)
                _drainTrackReports();
        });
    }
}

QVariantMap SwarmManager::perceptionDiagnostics() const
{
    return {{QStringLiteral("pending_reports"), QVariant::fromValue(quint64(_pendingReports.size()))},
            {QStringLiteral("dropped_reports"), QVariant::fromValue(_droppedReports)},
            {QStringLiteral("rejected_coordinates"), QVariant::fromValue(_rejectedCoordinates)},
            {QStringLiteral("fusion_rejected"), _fusion.rejectedCount()},
            {QStringLiteral("capacity_rejected"), QVariant::fromValue(_fusion.capacityRejectedCount())},
            {QStringLiteral("source_records"), _fusion.sourceCount()},
            {QStringLiteral("cooldown_records"), _eventDetector.cooldownRecordCount()},
            {QStringLiteral("speed_records"), _eventDetector.speedHistoryCount()}};
}

void SwarmManager::_onPerceptionTick()
{
    _constraintStore->refresh();
    const qint64 monoMs = SwarmClock::nowMs();
    for (const auto& execution : std::as_const(_executions)) {
        for (int id : execution->targets.keys()) {
            if (!execution->withdrawn.contains(id) && !_automationExcludedIds.contains(id))
                _lease.renewLease(id, monoMs);
        }
    }
    for (int id : _taskController->activeTargets()) {
        if (!_automationExcludedIds.contains(id))
            _lease.renewLease(id, monoMs);
    }
    _lease.expireCheck(monoMs);
    _fusion.prune(monoMs);

    const auto fused = _fusion.tracks();
    _copModel->updateFromFusion(fused, monoMs);

    // 覆盖热力：在线机体位置逐格累计
    const auto vehicles = _vehicleStates();
    for (const auto& v : vehicles) {
        if (v.online) {
            if (SwarmTypes::isFiniteLatLon(v.lat, v.lon))
                _copModel->ingestCoverage(v.lat, v.lon);
            else
                ++_rejectedCoordinates;
        }
    }

    // 事件检测：平台 + 目标
    _raiseAlerts(_eventDetector.checkVehicles(vehicles, monoMs));
    _raiseAlerts(_eventDetector.checkTracks(fused, monoMs));
    const auto overload = _droppedReports + _fusion.capacityRejectedCount() + _eventDetector.capacityRejectedCount();
    if (overload != _reportedOverloadCount && monoMs - _lastOverloadAlertMs >= 30000) {
        _reportedOverloadCount = overload;
        _lastOverloadAlertMs = monoMs;
        _raiseAlerts({{QStringLiteral("warning"),
                       QStringLiteral("perception_capacity"),
                       tr("感知处理达到容量上限，累计丢弃 %1 条新状态或排队报告").arg(overload),
                       {},
                       monoMs}});
    }
    emit perceptionDiagnosticsChanged();

    // 态势一行摘要（面板状态栏）
    const QJsonObject digest = SwarmSituationDigest::build(vehicles, fused, _taskController->stateText(),
                                                           _taskController->running(), _alerts, monoMs);
    const QString oneLiner = SwarmSituationDigest::oneLiner(digest);
    if (oneLiner != _situationOneLiner) {
        _situationOneLiner = oneLiner;
        emit situationChanged();
    }
}

void SwarmManager::_raiseAlerts(const QList<SwarmEventDetector::Alert>& alerts)
{
    if (alerts.isEmpty()) {
        return;
    }
    for (const auto& a : alerts) {
        _alerts.prepend(a);
        _alertsVariant.prepend(QVariantMap{
            {QStringLiteral("level"), a.level},
            {QStringLiteral("rule"), a.rule},
            {QStringLiteral("message"), a.message},
            {QStringLiteral("ids"), a.relatedIds},
            {QStringLiteral("text"), SwarmEventDetector::alertToText(a)},
        });
        emit swarmAlert(a.level, a.rule, a.message);
        SwarmAuditLogger::instance()->log(QStringLiteral("safety"), QStringLiteral("alert"),
                                          {
                                              {QStringLiteral("level"), a.level},
                                              {QStringLiteral("rule"), a.rule},
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
    const qint64 monoMs = SwarmClock::nowMs();
    const QJsonObject digest = SwarmSituationDigest::build(
        _vehicleStates(), _fusion.tracks(), _taskController->stateText(), _taskController->running(), _alerts, monoMs);
    return QString::fromUtf8(QJsonDocument(digest).toJson(QJsonDocument::Compact));
}

QList<Vehicle*> SwarmManager::_targetVehicles(bool requireActiveLease) const
{
    MultiVehicleManager* mvm = MultiVehicleManager::instance();
    // 优先作用于界面中选中的车辆，否则作用于全部在线车辆
    QmlObjectListModel* model =
        _dispatchTargetIds ? mvm->vehicles()
                           : ((mvm->selectedVehicles()->count() > 0) ? mvm->selectedVehicles() : mvm->vehicles());
    QList<Vehicle*> result;
    for (int i = 0; i < model->count(); i++) {
        Vehicle* vehicle = qobject_cast<Vehicle*>(model->get(i));
        if (vehicle && (!_dispatchTargetIds || _dispatchTargetIds->contains(vehicle->id()))) {
            result.append(vehicle);
        }
    }
    std::sort(result.begin(), result.end(), [](const Vehicle* a, const Vehicle* b) { return a->id() < b->id(); });

    // 逐机租约闸门：已被操作员手动接管（MANUAL_OVERRIDE）的机剔除，不再参与自动下发。
    QList<Vehicle*> gated;
    for (Vehicle* v : result) {
        if (!v || _lease.stateOf(v->id()) == SwarmControlLease::MANUAL_OVERRIDE) {
            continue;  // 已人工接管的机，跳过自动下发
        }
        if (requireActiveLease && !_lease.canAutomate(v->id(), SwarmClock::nowMs())) {
            continue;
        }
        gated.append(v);
    }
    return gated;
}

QList<int> SwarmManager::boundTargetIds() const
{
    QList<int> ids;
    for (Vehicle* vehicle : _targetVehicles())
        if (vehicle)
            ids.append(vehicle->id());
    return ids;
}

void SwarmManager::_forEachVehicleStaggered(const QString& commandName, int staggerMs,
                                            const std::function<void(Vehicle*, bool)>& action)
{
    if (!_automationDispatchInProgress) {
        _requestAutomationLeases(QStringLiteral("operator"));
    }
    const bool requireActiveLease = true;
    const QList<Vehicle*> targets = _targetVehicles(requireActiveLease);
    if (targets.isEmpty()) {
        emit swarmCommandFeedback(requireActiveLease ? tr("%1：当前没有持有有效租约的在线无人机").arg(commandName)
                                                     : tr("%1：当前没有在线无人机").arg(commandName));
        return;
    }

    const bool automated = _automationDispatchInProgress;
    int delayMs = 0;
    for (Vehicle* vehicle : targets) {
        QPointer<Vehicle> vehiclePtr(vehicle);
        const quint64 generation = _commandGenerations[vehicle->id()];
        QTimer::singleShot(
            delayMs, this, [this, vehiclePtr, action, commandName, requireActiveLease, automated, generation]() {
                if (_shutdownComplete || !vehiclePtr || generation != _commandGenerations.value(vehiclePtr->id()))
                    return;
                if (automated && aiExecutionBlocked())
                    return;
                if (vehiclePtr && _automationAllowedAtDispatch(vehiclePtr->id(), requireActiveLease, commandName)) {
                    action(vehiclePtr.data(), requireActiveLease);
                }
            });
        delayMs += staggerMs;
    }
    emit swarmCommandFeedback(tr("%1：已排队下发至 %2 架无人机").arg(commandName).arg(targets.count()));
}

void SwarmManager::_requestAutomationLeases(const QString& origin)
{
    const qint64 nowMs = SwarmClock::nowMs();
    const bool operatorAuthorized = originHasOperatorAuthorization(origin, _taskController);
    for (Vehicle* vehicle : _targetVehicles()) {
        if (vehicle) {
            if (!operatorAuthorized && _lease.canAutomate(vehicle->id(), nowMs)) {
                _lease.renewLease(vehicle->id(), nowMs);
                continue;
            }
            const QString token =
                _lease.requestLeaseForMode(vehicle->id(), origin, QStringLiteral("swarm"), nowMs, vehicle->flightMode(),
                                           _autonomousModes(vehicle), operatorAuthorized);
            if (token.isEmpty() && _lease.stateOf(vehicle->id()) == SwarmControlLease::MANUAL_OVERRIDE) {
                SwarmAuditLogger::instance()->log(QStringLiteral("safety"),
                                                  QStringLiteral("lease_request_blocked_by_mode"),
                                                  {
                                                      {QStringLiteral("origin"), origin},
                                                      {QStringLiteral("vehicle_id"), vehicle->id()},
                                                      {QStringLiteral("flight_mode"), vehicle->flightMode()},
                                                  });
            }
        }
    }
}

bool SwarmManager::_automationAllowedAtDispatch(int vehicleId, bool requireActiveLease, const QString& commandName)
{
    if (!requireActiveLease || _lease.canAutomate(vehicleId, SwarmClock::nowMs())) {
        return true;
    }

    emit swarmCommandFeedback(tr("%1：%2 号机租约已失效或被人工接管，已跳过").arg(commandName).arg(vehicleId));
    SwarmAuditLogger::instance()->log(QStringLiteral("safety"), QStringLiteral("lease_dispatch_skipped"),
                                      {
                                          {QStringLiteral("command"), commandName},
                                          {QStringLiteral("vehicle_id"), vehicleId},
                                      });
    return false;
}

bool SwarmManager::_formationAutomationAllowed(int vehicleId)
{
    if (!_formationTargetIds.contains(vehicleId) || (_formationAutomated && aiExecutionBlocked()))
        return false;
    const qint64 nowMs = SwarmClock::nowMs();
    Vehicle* vehicle = MultiVehicleManager::instance()->getVehicleById(vehicleId);
    if (!vehicle) {
        return false;
    }
    const SwarmControlLease::State state = _lease.stateOf(vehicleId);
    if (state == SwarmControlLease::OBSERVE_ONLY || state == SwarmControlLease::RELEASED) {
        _lease.requestLeaseForMode(vehicleId, QStringLiteral("formation"), QStringLiteral("swarm"), nowMs,
                                   vehicle->flightMode(), _autonomousModes(vehicle), false /*operatorAuthorized*/);
    }
    if (!_lease.canAutomate(vehicleId, nowMs)) {
        return false;
    }
    return _lease.renewLease(vehicleId, nowMs);
}

void SwarmManager::armAll(bool arm)
{
    if (!_automationDispatchInProgress) {
        for (int id : boundTargetIds())
            _automationExcludedIds.remove(id);
    }
    for (Vehicle* vehicle : _targetVehicles())
        if (vehicle)
            ++_commandGenerations[vehicle->id()];
    SwarmAuditLogger::instance()->logOperatorCommand(arm ? QStringLiteral("armAll") : QStringLiteral("disarmAll"));
    _forEachVehicleStaggered(arm ? tr("集群解锁") : tr("集群上锁"), _commandIntervalMs,
                             [arm](Vehicle* vehicle, bool) { vehicle->setArmed(arm, true /*showError*/); });
}

void SwarmManager::takeoffAll(double altitudeRelative)
{
    SwarmAction action;
    action.type = QStringLiteral("takeoff");
    action.params = {{QStringLiteral("altitude"), altitudeRelative}};
    for (Vehicle* vehicle : _targetVehicles())
        if (vehicle)
            action.targetIds.append(vehicle->id());
    if (!std::isfinite(altitudeRelative) || altitudeRelative < 2 || altitudeRelative > 120 ||
        planHasConstraintError({action})) {
        emit swarmCommandFeedback(tr("起飞被拒绝：高度或实时约束无效"));
        return;
    }
    executeActions({action}, QStringLiteral("operator"), ExecutionApproval::OperatorConfirmed);
}

void SwarmManager::landAll()
{
    _cancelSelectedTargets(QStringLiteral("landAll"));
    SwarmAuditLogger::instance()->logOperatorCommand(QStringLiteral("landAll"));
    _forEachVehicleStaggered(tr("集群降落"), _commandIntervalMs,
                             [](Vehicle* vehicle, bool) { vehicle->guidedModeLand(); });
}

void SwarmManager::rtlAll()
{
    _cancelSelectedTargets(QStringLiteral("rtlAll"));
    SwarmAuditLogger::instance()->logOperatorCommand(QStringLiteral("rtlAll"));
    _forEachVehicleStaggered(tr("集群返航"), _commandIntervalMs,
                             [](Vehicle* vehicle, bool) { vehicle->guidedModeRTL(true /*smartRTL*/); });
}

void SwarmManager::pauseAll()
{
    _cancelSelectedTargets(QStringLiteral("pauseAll"));
    SwarmAuditLogger::instance()->logOperatorCommand(QStringLiteral("pauseAll"));
    _forEachVehicleStaggered(tr("集群暂停"), 0 /*立即同时执行*/,
                             [](Vehicle* vehicle, bool) { vehicle->pauseVehicle(); });
}

void SwarmManager::pauseTargets(const QList<int>& ids)
{
    QScopedValueRollback<bool> dispatchScope(_automationDispatchInProgress, true);
    QScopedValueRollback<std::optional<QList<int>>> targetsScope(_dispatchTargetIds, ids);
    pauseAll();
}

void SwarmManager::_cancelSelectedTargets(const QString& reason)
{
    QList<int> ids;
    for (Vehicle* vehicle : _targetVehicles()) {
        if (vehicle)
            ids.append(vehicle->id());
    }
    if (!_automationDispatchInProgress) {
        cancelTargets(ids, reason);
    } else {
        for (int id : ids)
            ++_commandGenerations[id];
    }
}

void SwarmManager::cancelTargets(const QList<int>& ids, const QString& reason)
{
    if (ids.isEmpty())
        return;
    for (int id : ids) {
        ++_commandGenerations[id];
        _automationExcludedIds.insert(id);
        _formationTargetIds.remove(id);
    }
    for (const auto& execution : std::as_const(_executions)) {
        for (int id : ids)
            execution->withdrawn.insert(id);
    }
    _formation->removeMembers(QSet<int>(ids.begin(), ids.end()));
    _missionPlanner->cancelTargets(ids);
    _aiAgent->invalidatePendingWork();
    SwarmAuditLogger::instance()->logTaskEvent(
        QStringLiteral("members_withdrawn"),
        {{QStringLiteral("reason"), reason}, {QStringLiteral("count"), ids.size()}});
    // A terminal task notification may start a new task. Finish cancelling the old work first.
    _taskController->withdrawTargets(ids, reason);
}

void SwarmManager::startMissionAll()
{
    SwarmAction action;
    action.type = QStringLiteral("start_mission");
    for (Vehicle* vehicle : _targetVehicles())
        if (vehicle)
            action.targetIds.append(vehicle->id());
    executeActions({action}, QStringLiteral("operator"), ExecutionApproval::OperatorConfirmed);
}

void SwarmManager::authorizeTaskTargets(const QList<int>& ids)
{
    if (_shutdownComplete || aiExecutionBlocked())
        return;
    QScopedValueRollback<std::optional<QList<int>>> targetsScope(_dispatchTargetIds, ids);
    for (int id : ids)
        _automationExcludedIds.remove(id);
    _requestAutomationLeases(QStringLiteral("ai_confirmed"));
}

void SwarmManager::emergencyDisarmAll()
{
    _cancelSelectedTargets(QStringLiteral("emergencyDisarmAll"));
    SwarmAuditLogger::instance()->logOperatorCommand(QStringLiteral("emergencyDisarmAll"));
    _forEachVehicleStaggered(tr("紧急上锁"), 0, [](Vehicle* vehicle, bool) {
        // MAV_CMD_COMPONENT_ARM_DISARM: param1=0(上锁), param2=21196(强制，含空中)
        vehicle->sendMavCommand(vehicle->defaultComponentId(), MAV_CMD_COMPONENT_ARM_DISARM, true /*showError*/,
                                0.0f /*disarm*/, 21196.0f /*force*/);
    });
}

bool SwarmManager::startFormation(int leaderVehicleId)
{
    SwarmAction action;
    action.type = QStringLiteral("start_formation");
    action.params = {{QStringLiteral("spacing"), _formation->spacingMeters()}};
    for (Vehicle* vehicle : _targetVehicles())
        if (vehicle)
            action.targetIds.append(vehicle->id());
    if (!action.targetIds.contains(leaderVehicleId) || planHasConstraintError({action})) {
        emit swarmCommandFeedback(tr("编队被拒绝：目标集或实时约束无效"));
        return false;
    }
    _formationAutomated = false;
    for (int id : action.targetIds)
        _automationExcludedIds.remove(id);
    _formationTargetIds = QSet<int>(action.targetIds.cbegin(), action.targetIds.cend());
    _requestAutomationLeases(QStringLiteral("operator"));
    _formation->setMembers(_formationTargetIds);
    return _formation->start(leaderVehicleId);
}

void SwarmManager::stopFormation()
{
    _formation->stop();
}

bool SwarmManager::planAndUploadCoverage(const QGeoCoordinate& center, double lengthMeters, double widthMeters,
                                         double angleDeg, const QString& assignMode)
{
    QStringList errors;
    const auto actions =
        parseAndValidate(QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("plan_coverage")},
                                                {QStringLiteral("center_lat"), center.latitude()},
                                                {QStringLiteral("center_lon"), center.longitude()},
                                                {QStringLiteral("length"), lengthMeters},
                                                {QStringLiteral("width"), widthMeters},
                                                {QStringLiteral("angle"), angleDeg},
                                                {QStringLiteral("altitude"), _missionPlanner->surveyAltitude()},
                                                {QStringLiteral("spacing"), _missionPlanner->surveySpacing()},
                                                {QStringLiteral("assign"), assignMode}}},
                         center, errors);
    if (actions.isEmpty() || planHasConstraintError(actions)) {
        emit swarmCommandFeedback(tr("覆盖规划被拒绝：%1").arg(errors.join(QStringLiteral("；"))));
        return false;
    }
    QScopedValueRollback<std::optional<QList<int>>> targetsScope(_dispatchTargetIds, actions.first().targetIds);
    _requestAutomationLeases(QStringLiteral("operator"));
    return _missionPlanner->planAndUploadCoverage(center, lengthMeters, widthMeters, angleDeg, assignMode);
}

bool SwarmManager::clearAllMissions()
{
    SwarmAction action;
    action.type = QStringLiteral("clear_missions");
    action.targetIds = boundTargetIds();
    return executeActions({action}, QStringLiteral("operator"), ExecutionApproval::OperatorConfirmed);
}

// ---------------------------------------------------------------------------
// 统一动作管线：解析校验（大模型 / 任务状态机 / 人工界面共用）
// ---------------------------------------------------------------------------
QList<SwarmAction> SwarmManager::parseAndValidate(const QJsonArray& actionsJson, const QGeoCoordinate& mapCenter,
                                                  QStringList& warnings, const QList<int>& initialTargets) const
{
    static const QSet<QString> knownTypes = {QStringLiteral("set_selection"),
                                             QStringLiteral("arm"),
                                             QStringLiteral("takeoff"),
                                             QStringLiteral("land"),
                                             QStringLiteral("rtl"),
                                             QStringLiteral("pause"),
                                             QStringLiteral("start_mission"),
                                             QStringLiteral("disarm"),
                                             QStringLiteral("emergency_disarm"),
                                             QStringLiteral("start_formation"),
                                             QStringLiteral("stop_formation"),
                                             QStringLiteral("plan_coverage"),
                                             QStringLiteral("clear_missions"),
                                             QStringLiteral("plan_tactic")};

    QList<SwarmAction> plan;
    MultiVehicleManager* mvm = MultiVehicleManager::instance();

    QList<int> online;
    for (int i = 0; i < mvm->vehicles()->count(); i++) {
        Vehicle* vehicle = qobject_cast<Vehicle*>(mvm->vehicles()->get(i));
        if (vehicle) {
            online.append(vehicle->id());
        }
    }

    if (actionsJson.size() > 10) {
        warnings.append(tr("动作数超过 10，整个计划已拒绝"));
        return {};
    }
    QList<int> targets = initialTargets;
    if (targets.isEmpty()) {
        QmlObjectListModel* selected = mvm->selectedVehicles()->count() > 0 ? mvm->selectedVehicles() : mvm->vehicles();
        for (int i = 0; i < selected->count(); ++i) {
            Vehicle* vehicle = qobject_cast<Vehicle*>(selected->get(i));
            if (vehicle)
                targets.append(vehicle->id());
        }
    }
    const int actionCount = actionsJson.count();  // 单次最多执行 10 个动作，防失控输出
    for (int i = 0; i < actionCount; i++) {
        const QJsonObject obj = actionsJson.at(i).toObject();
        const QString type = obj.value(QStringLiteral("type")).toString();
        if (!knownTypes.contains(type)) {
            warnings.append(tr("已忽略未知动作类型：%1").arg(type));
            return {};
        }

        const QString inputError = validateActionInput(obj);
        if (!inputError.isEmpty()) {
            warnings.append(tr("动作 %1：%2；整个计划已拒绝").arg(i + 1).arg(inputError));
            return {};
        }
        SwarmAction action;
        action.type = type;
        action.dangerous = SwarmAction::isDangerousType(type);

        if (type == QStringLiteral("set_selection")) {
            QList<int> ids;
            for (const QJsonValue& v : obj.value(QStringLiteral("vehicle_ids")).toArray()) {
                const int id = v.toInt(-1);
                if (online.contains(id)) {
                    if (!ids.contains(id))
                        ids.append(id);
                } else {
                    warnings.append(tr("机号 %1 不在线，整个计划已拒绝").arg(id));
                    return {};
                }
            }
            if (ids.isEmpty()) {
                warnings.append(tr("set_selection 无有效机号，已忽略"));
                return {};
            }
            targets = ids;
            action.params[QStringLiteral("vehicle_ids")] = QVariant::fromValue(ids);
            QStringList idStrings;
            for (int id : ids) {
                idStrings.append(QString::number(id));
            }
            action.description = tr("选中 %1 号机").arg(idStrings.join(QStringLiteral("、")));
        } else if (type == QStringLiteral("arm")) {
            const bool arm = obj.value(QStringLiteral("arm")).toBool();
            action.params[QStringLiteral("arm")] = arm;
            action.description = arm ? tr("集群解锁") : tr("集群上锁");
        } else if (type == QStringLiteral("takeoff")) {
            const double altRaw = obj.value(QStringLiteral("altitude")).toDouble(30.0);
            const double alt = qBound(2.0, altRaw, 120.0);
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
                return {};
            }
            static const QMap<QString, QString> typeNameMap = {{QStringLiteral("wedge"), tr("楔形")},
                                                               {QStringLiteral("column"), tr("纵队")},
                                                               {QStringLiteral("line"), tr("横队")},
                                                               {QStringLiteral("circle"), tr("环形")}};
            QString formationName = obj.value(QStringLiteral("formation")).toString(QStringLiteral("wedge"));
            if (!typeNameMap.contains(formationName)) {
                warnings.append(tr("未知队形 %1，已按楔形处理").arg(formationName));
                formationName = QStringLiteral("wedge");
            }
            const double spacing = qBound(2.0, obj.value(QStringLiteral("spacing")).toDouble(10.0), 500.0);
            const double vstep = qBound(0.0, obj.value(QStringLiteral("vertical_step")).toDouble(3.0), 50.0);
            action.params[QStringLiteral("leader_id")] = leaderId;
            action.params[QStringLiteral("formation")] = formationName;
            action.params[QStringLiteral("spacing")] = spacing;
            action.params[QStringLiteral("vertical_step")] = vstep;
            action.description = tr("以 %1 号为长机开始%2编队（间距 %3 米，分层 %4 米）")
                                     .arg(leaderId)
                                     .arg(typeNameMap.value(formationName))
                                     .arg(spacing)
                                     .arg(vstep);
        } else if (type == QStringLiteral("stop_formation")) {
            action.description = tr("停止编队");
        } else if (type == QStringLiteral("plan_coverage")) {
            double centerLat = mapCenter.latitude(), centerLon = mapCenter.longitude();
            if (obj.contains(QStringLiteral("center_lat"))) {
                centerLat = obj.value(QStringLiteral("center_lat")).toDouble();
                centerLon = obj.value(QStringLiteral("center_lon")).toDouble();
            }
            const QJsonValue centerVal = obj.value(QStringLiteral("center"));
            if (centerVal.isArray() && centerVal.toArray().count() >= 2) {
                centerLat = centerVal.toArray().at(0).toDouble();
                centerLon = centerVal.toArray().at(1).toDouble();
            }
            const QGeoCoordinate center(centerLat, centerLon);
            if (!center.isValid()) {
                warnings.append(tr("区域中心坐标无效，任务规划动作已忽略"));
                return {};
            }
            const double length = qBound(10.0, obj.value(QStringLiteral("length")).toDouble(200.0), 100000.0);
            const double width = qBound(10.0, obj.value(QStringLiteral("width")).toDouble(120.0), 100000.0);
            const double angle = qBound(0.0, obj.value(QStringLiteral("angle")).toDouble(0.0), 360.0);
            const double spacing = qBound(2.0, obj.value(QStringLiteral("spacing")).toDouble(20.0), 1000.0);
            const double alt = qBound(3.0, obj.value(QStringLiteral("altitude")).toDouble(30.0), 120.0);
            // WP2.4：条带分配方式（sequential 默认 / auction 集中式拍卖 / cbba 分布式一致性拍卖）
            const QString assignRaw = obj.value(QStringLiteral("assign")).toString();
            QString assignMode = QStringLiteral("sequential");
            if (assignRaw == QStringLiteral("auction") || assignRaw == QStringLiteral("cbba") ||
                assignRaw == QStringLiteral("sequential")) {
                assignMode = assignRaw;
            } else if (!assignRaw.isEmpty()) {
                warnings.append(tr("未知分配方式「%1」，已回退为顺序分配").arg(assignRaw));
            }
            action.params[QStringLiteral("center_lat")] = centerLat;
            action.params[QStringLiteral("center_lon")] = centerLon;
            action.params[QStringLiteral("length")] = length;
            action.params[QStringLiteral("width")] = width;
            action.params[QStringLiteral("angle")] = angle;
            action.params[QStringLiteral("spacing")] = spacing;
            action.params[QStringLiteral("altitude")] = alt;
            action.params[QStringLiteral("assign")] = assignMode;
            action.description = tr("区域覆盖任务规划：%1×%2 米，角度 %3°，行距 %4 米，高度 %5 米%6")
                                     .arg(length)
                                     .arg(width)
                                     .arg(angle)
                                     .arg(spacing)
                                     .arg(alt)
                                     .arg(assignMode == QStringLiteral("auction") ? tr("（集中拍卖分配）")
                                          : assignMode == QStringLiteral("cbba")  ? tr("（CBBA 分布式分配）")
                                                                                  : QString());
        } else if (type == QStringLiteral("clear_missions")) {
            action.description = tr("清空各机任务");
        } else if (type == QStringLiteral("plan_tactic")) {
            // 协同战术规划：跟踪/围捕/拦截/巡逻（规划建议级，输出方案供操作员确认）
            static const QSet<QString> kScenarios = {QStringLiteral("track"), QStringLiteral("encircle"),
                                                     QStringLiteral("intercept"), QStringLiteral("patrol")};
            const QString scenario = obj.value(QStringLiteral("scenario")).toString();
            if (!kScenarios.contains(scenario)) {
                warnings.append(tr("未知战术场景「%1」，已忽略").arg(scenario));
                return {};
            }
            action.params[QStringLiteral("scenario")] = scenario;
            if (scenario == QStringLiteral("patrol")) {
                QVariantList poly;
                for (const QJsonValue& pt : obj.value(QStringLiteral("polygon")).toArray()) {
                    const QJsonArray a = pt.toArray();
                    if (a.count() >= 2 && SwarmTypes::isValidLatLon(a.at(0).toDouble(), a.at(1).toDouble())) {
                        poly.append(QVariant(QVariantList{a.at(0).toDouble(), a.at(1).toDouble()}));
                    }
                }
                if (poly.count() < 3) {
                    warnings.append(tr("巡逻多边形至少需 3 个顶点，已忽略"));
                    return {};
                }
                action.params[QStringLiteral("polygon")] = poly;
                action.description = tr("巡逻规划：%1 顶点周界均分").arg(poly.count());
            } else {
                const double tLat = obj.value(QStringLiteral("target_lat")).toDouble();
                const double tLon = obj.value(QStringLiteral("target_lon")).toDouble();
                if (!SwarmTypes::isValidLatLon(tLat, tLon)) {  // 拦截 LLM 幻觉/缺失坐标
                    warnings.append(tr("战术场景「%1」目标坐标非法或缺失，已忽略").arg(scenario));
                    return {};
                }
                action.params[QStringLiteral("target_lat")] = tLat;
                action.params[QStringLiteral("target_lon")] = tLon;
                if (scenario == QStringLiteral("intercept")) {
                    action.params[QStringLiteral("target_speed")] =
                        qBound(0.0, obj.value(QStringLiteral("target_speed")).toDouble(0.0), 340.0);
                    const double hdg = std::fmod(obj.value(QStringLiteral("target_heading")).toDouble(0.0), 360.0);
                    action.params[QStringLiteral("target_heading")] = (hdg < 0.0) ? hdg + 360.0 : hdg;
                    action.params[QStringLiteral("intercept_speed")] =
                        qBound(1.0, obj.value(QStringLiteral("intercept_speed")).toDouble(20.0), 100.0);
                    action.description = tr("拦截规划：预测碰撞拦截点");
                } else {
                    action.params[QStringLiteral("radius")] =
                        qBound(10.0, obj.value(QStringLiteral("radius")).toDouble(150.0), 5000.0);
                    action.description = (scenario == QStringLiteral("track")) ? tr("环绕跟踪规划：定半径观测")
                                                                               : tr("包围规划：环形合围");
                }
            }
        }

        action.targetIds = targets;
        if (type != QStringLiteral("stop_formation") && targets.isEmpty()) {
            warnings.append(tr("没有目标机，整个计划已拒绝"));
            return {};
        }
        if (type == QStringLiteral("start_formation") &&
            !targets.contains(action.params.value(QStringLiteral("leader_id")).toInt())) {
            warnings.append(tr("长机不在目标集合内"));
            return {};
        }
        if (type != QStringLiteral("set_selection") && !targets.isEmpty()) {
            QStringList names;
            for (int id : targets)
                names.append(QString::number(id));
            action.description += tr(" [目标机：%1]").arg(names.join(QStringLiteral(", ")));
        }
        plan.append(action);
    }
    return plan;
}

// ---------------------------------------------------------------------------
// 统一动作管线：执行
// ---------------------------------------------------------------------------
bool SwarmManager::_executeImmediateActions(const QList<SwarmAction>& actions, const QString& origin,
                                            ExecutionApproval approval)
{
    // 纵深防御兜底闸门:任何入口(含 task 状态机自动推进)在 L1 档位一律拒绝下发,
    // 防止运行中降级自主等级或新增入口绕过前置 autonomy 闸门。
    QString gateReason;
    if (aiExecutionBlocked(&gateReason)) {
        emit swarmCommandFeedback(tr("下发被安全闸门拦截：%1（来源 %2）").arg(gateReason, origin));
        SwarmAuditLogger::instance()->log(QStringLiteral("safety"), QStringLiteral("execution_gate_blocked"),
                                          {{QStringLiteral("reason"), gateReason},
                                           {QStringLiteral("origin"), origin},
                                           {QStringLiteral("actions"), actions.count()}});
        return false;
    }

    if ((approval == ExecutionApproval::Automatic && needsConfirmation(actions, &gateReason)) ||
        planHasConstraintError(actions)) {
        emit swarmCommandFeedback(tr("计划未下发：缺少确认或实时约束未通过。%1").arg(gateReason));
        return false;
    }
    MultiVehicleManager* mvm = MultiVehicleManager::instance();

    SwarmAuditLogger::instance()->log(QStringLiteral("exec"), QStringLiteral("action_sequence"),
                                      {{QStringLiteral("origin"), origin}, {QStringLiteral("count"), actions.count()}});

    QScopedValueRollback<bool> dispatchScope(_automationDispatchInProgress, true);
    for (const SwarmAction& action : actions) {
        QScopedValueRollback<std::optional<QList<int>>> targetsScope(_dispatchTargetIds, action.targetIds);
        const QString& type = action.type;
        const QVariantMap& p = action.params;

        SwarmAuditLogger::instance()->logActionDispatch(type, p, action.description);

        if (type == QStringLiteral("set_selection")) {
            mvm->deselectAllVehicles();
            for (const QVariant& v : p.value(QStringLiteral("vehicle_ids")).toList()) {
                mvm->selectVehicle(v.toInt());
            }
            continue;
        }

        // 每项动作按解析时绑定的集合申请/刷新租约。
        const bool authorizeLease = approval == ExecutionApproval::OperatorConfirmed &&
                                    (origin != QStringLiteral("task") || _taskController->currentStageIndex() == 0);
        _requestAutomationLeases(authorizeLease ? QStringLiteral("ai_confirmed") : QStringLiteral("ai"));
        if (type == QStringLiteral("start_formation")) {
            const QString name = p.value(QStringLiteral("formation")).toString();
            FormationController::FormationType formationType = FormationController::FormationWedge;
            if (name == QStringLiteral("column"))
                formationType = FormationController::FormationColumn;
            else if (name == QStringLiteral("line"))
                formationType = FormationController::FormationLine;
            else if (name == QStringLiteral("circle"))
                formationType = FormationController::FormationCircle;
            _formation->setFormationType(formationType);
            _formation->setSpacingMeters(p.value(QStringLiteral("spacing")).toDouble());
            _formation->setVerticalStepMeters(p.value(QStringLiteral("vertical_step")).toDouble());
            _formationAutomated = true;
            _formationApproval = approval;
            _formationTargetIds = QSet<int>(action.targetIds.cbegin(), action.targetIds.cend());
            _formation->setMembers(_formationTargetIds);
            if (!_formation->start(p.value(QStringLiteral("leader_id")).toInt()))
                return false;
        } else if (type == QStringLiteral("stop_formation")) {
            _formation->stop();
        } else if (type == QStringLiteral("clear_missions")) {
            _missionPlanner->clearAllMissions();
        } else if (type == QStringLiteral("plan_tactic")) {
            _executeTactic(p);
        } else {
            return false;
        }
    }
    emit swarmCommandFeedback(tr("动作序列已受理（%1 项）").arg(actions.count()));
    return true;
}

// ---------------------------------------------------------------------------
// 协同战术规划：跟踪/围捕/拦截/巡逻（规划建议级 L1，输出方案经 swarmCommandFeedback）
// ---------------------------------------------------------------------------
void SwarmManager::_executeTactic(const QVariantMap& p)
{
    const QString scenario = p.value(QStringLiteral("scenario")).toString();
    const QList<SwarmTypes::VehicleState> states = _vehicleStates(_dispatchTargetIds);
    if (states.isEmpty()) {
        emit swarmCommandFeedback(tr("战术规划：当前没有可用无人机"));
        return;
    }

    if (scenario == QStringLiteral("track")) {
        const double R = p.value(QStringLiteral("radius")).toDouble();
        const QList<SwarmTactics::GeoPoint> ring =
            SwarmTactics::standoffRing(p.value(QStringLiteral("target_lat")).toDouble(),
                                       p.value(QStringLiteral("target_lon")).toDouble(), R, states.count());
        emit swarmCommandFeedback(tr("环绕跟踪规划：%1 机以半径 %2 米相位均分观测（首点 %3, %4）")
                                      .arg(ring.count())
                                      .arg(R, 0, 'f', 0)
                                      .arg(ring.isEmpty() ? 0.0 : ring.first().lat, 0, 'f', 6)
                                      .arg(ring.isEmpty() ? 0.0 : ring.first().lon, 0, 'f', 6));
    } else if (scenario == QStringLiteral("encircle")) {
        const double R = p.value(QStringLiteral("radius")).toDouble();
        const QMap<int, SwarmTactics::GeoPoint> asg =
            SwarmTactics::encircle(states, p.value(QStringLiteral("target_lat")).toDouble(),
                                   p.value(QStringLiteral("target_lon")).toDouble(), R);
        QStringList pts;  // 回传实际合围点坐标(前若干个),让方案可见而非只报计数
        for (auto it = asg.constBegin(); it != asg.constEnd() && pts.size() < 3; ++it) {
            pts.append(tr("%1号→(%2, %3)").arg(it.key()).arg(it.value().lat, 0, 'f', 6).arg(it.value().lon, 0, 'f', 6));
        }
        emit swarmCommandFeedback(tr("包围规划：%1 机在半径 %2 米环上就近分配合围点；%3%4")
                                      .arg(asg.count())
                                      .arg(R, 0, 'f', 0)
                                      .arg(pts.join(QStringLiteral("、")))
                                      .arg(asg.count() > 3 ? tr(" 等") : QString()));
    } else if (scenario == QStringLiteral("intercept")) {
        SwarmTactics::MovingTarget tgt;
        tgt.lat = p.value(QStringLiteral("target_lat")).toDouble();
        tgt.lon = p.value(QStringLiteral("target_lon")).toDouble();
        tgt.speedMps = p.value(QStringLiteral("target_speed")).toDouble();
        tgt.headingDeg = p.value(QStringLiteral("target_heading")).toDouble();
        const int insId = SwarmTactics::pickInterceptor(states, tgt.lat, tgt.lon);  // 选距目标最近机
        const SwarmTypes::VehicleState* insPtr = nullptr;
        for (const SwarmTypes::VehicleState& vs : states) {
            if (vs.id == insId) {
                insPtr = &vs;
                break;
            }
        }
        const SwarmTypes::VehicleState& ins = insPtr ? *insPtr : states.first();
        const double v = p.value(QStringLiteral("intercept_speed")).toDouble();
        const SwarmTactics::InterceptSolution sol = SwarmTactics::intercept(ins.lat, ins.lon, v, tgt);
        if (sol.feasible) {
            emit swarmCommandFeedback(tr("拦截规划：%1 号机航向 %2°、约 %3 秒于 (%4, %5) 拦截目标")
                                          .arg(ins.id)
                                          .arg(sol.headingDeg, 0, 'f', 0)
                                          .arg(sol.timeS, 0, 'f', 0)
                                          .arg(sol.lat, 0, 'f', 6)
                                          .arg(sol.lon, 0, 'f', 6));
        } else {
            emit swarmCommandFeedback(tr("拦截规划：%1 号机速度不足，无法拦截该目标").arg(ins.id));
        }
    } else if (scenario == QStringLiteral("patrol")) {
        QList<QPair<double, double>> poly;
        for (const QVariant& pt : p.value(QStringLiteral("polygon")).toList()) {
            const QVariantList xy = pt.toList();
            if (xy.count() >= 2) {
                poly.append({xy.at(0).toDouble(), xy.at(1).toDouble()});
            }
        }
        const QMap<int, QList<SwarmTactics::GeoPoint>> plan = SwarmTactics::partitionPatrol(states, poly);
        int totalPts = 0;
        QStringList segs;  // 回传各机巡逻段的起点坐标(前若干个)
        for (auto it = plan.constBegin(); it != plan.constEnd(); ++it) {
            totalPts += it.value().count();
            if (segs.size() < 3 && !it.value().isEmpty()) {
                segs.append(tr("%1号守%2点自(%3, %4)")
                                .arg(it.key())
                                .arg(it.value().count())
                                .arg(it.value().first().lat, 0, 'f', 6)
                                .arg(it.value().first().lon, 0, 'f', 6));
            }
        }
        emit swarmCommandFeedback(tr("巡逻规划：%1 机沿 %2 边周界均分共 %3 巡逻点；%4")
                                      .arg(plan.count())
                                      .arg(poly.count())
                                      .arg(totalPts)
                                      .arg(segs.join(QStringLiteral("、"))));
    }
}

void SwarmManager::_vehicleAdded(Vehicle* vehicle)
{
    if (vehicle) {
        const auto updateLeaseFromMode = [this, vehicle](const QString& mode) {
            // 用 Vehicle 规范飞行模式访问器(与 flightMode() 同 tr() 上下文,locale 安全)
            // 构成自主模式集；非空集合外模式（操作员手动直控 / Offboard 外部控制）让出租约。
            // 空模式是遥测切换瞬态，不作为人工接管证据。
            // 切回自主模式即解除接管(可恢复)。
            const bool autonomous = SwarmControlLease::isAutonomousMode(mode, _autonomousModes(vehicle));
            if (autonomous) {
                _lease.setManualOverride(vehicle->id(), false);
            } else if (SwarmControlLease::indicatesManualOverride(mode, _autonomousModes(vehicle)) &&
                       _lease.stateOf(vehicle->id()) == SwarmControlLease::LEASED) {
                // 仅在自动化已持有租约时，集合外模式切换才表示操作员接管。
                _lease.setManualOverride(vehicle->id(), true);
                cancelTargets({vehicle->id()}, tr("操作员切换到人工控制模式"));
            }
        };
        connect(vehicle, &Vehicle::flightModeChanged, this, updateLeaseFromMode);
        updateLeaseFromMode(vehicle->flightMode());
    }
    emit vehicleCountChanged();
}

QStringList SwarmManager::_autonomousModes(Vehicle* vehicle) const
{
    if (!vehicle) {
        return {};
    }
    QStringList modes = {
        vehicle->missionFlightMode(),  vehicle->pauseFlightMode(), vehicle->rtlFlightMode(),
        vehicle->smartRTLFlightMode(), vehicle->landFlightMode(),  vehicle->followFlightMode(),
        vehicle->gotoFlightMode(),
    };
    if (FirmwarePlugin* firmwarePlugin = vehicle->firmwarePlugin()) {
        modes.append(firmwarePlugin->takeOffFlightMode());
    }
    return modes;
}

void SwarmManager::_vehicleRemoved(Vehicle* vehicle)
{
    if (vehicle) {
        cancelTargets({vehicle->id()}, tr("载具断开"));
        _lease.forgetVehicle(vehicle->id());
    }
    emit vehicleCountChanged();
}

bool SwarmManager::executeActions(const QList<SwarmAction>& actions, const QString& origin, ExecutionApproval approval,
                                  const QHash<int, SwarmMissionSnapshot>& expectedMissions, quint64* executionId)
{
    const bool manual = origin == QStringLiteral("operator");
    QString reason;
    if (_shutdownComplete || (!manual && aiExecutionBlocked(&reason)) ||
        (approval == ExecutionApproval::Automatic && needsConfirmation(actions, &reason)) ||
        planHasConstraintError(actions))
        return false;
    // A separately submitted recovery instruction retires earlier task ownership too.
    // Do this before allocating its context so it cannot cancel itself.
    if (origin != QStringLiteral("task")) {
        QSet<int> retiring;
        for (const auto& action : actions) {
            if (action.type == QStringLiteral("pause") || action.type == QStringLiteral("rtl") ||
                action.type == QStringLiteral("land") || action.type == QStringLiteral("emergency_disarm"))
                retiring.unite(QSet<int>(action.targetIds.begin(), action.targetIds.end()));
        }
        cancelTargets(retiring.values(), tr("新恢复指令替代已有自动任务"));
    }
    auto execution = std::make_shared<Execution>();
    execution->id = ++_lastExecutionId;
    if (executionId)
        *executionId = execution->id;
    execution->policyRevision = _policyRevision;
    _executionStates.clear();
    _executionMessage.clear();
    execution->actions = actions;
    execution->origin = origin;
    execution->approval = approval;
    execution->plannedMissions = expectedMissions;
    execution->context = new QObject(this);
    for (const auto& action : actions) {
        for (int id : action.targetIds) {
            const bool newOperatorInstruction =
                approval == ExecutionApproval::OperatorConfirmed &&
                (origin != QStringLiteral("task") || _taskController->currentStageIndex() == 0);
            if (newOperatorInstruction)
                _automationExcludedIds.remove(id);
            execution->targets[id] = MultiVehicleManager::instance()->getVehicleById(id);
            execution->generations[id] = _commandGenerations[id];
            _executionStates[QString::number(id)] = QStringLiteral("pending");
        }
    }
    _executions.insert(execution->id, execution);
    connect(this, &SwarmManager::executionProgress, execution->context,
            [this, id = execution->id](quint64 eventId, int vehicleId, const QString& state) {
                if (eventId != id || id != _lastExecutionId)
                    return;
                _executionStates[QString::number(vehicleId)] = state;
                emit executionStatesChanged();
            });
    emit executionStatesChanged();
    _continueExecution(execution);
    return true;
}

void SwarmManager::_finishExecution(const std::shared_ptr<Execution>& execution, bool success, const QString& reason)
{
    if (execution->finished)
        return;
    execution->finished = true;
    if (!success && execution->failureCode.isEmpty())
        execution->failureCode = QStringLiteral("execution_failed");
    if (execution->plannerBatch != 0 && execution->plannerBatch == _missionPlanner->batchId())
        _missionPlanner->cancelTargets(execution->targets.keys());
    if (execution->id == _lastExecutionId) {
        _executionMessage = reason;
        emit executionStatesChanged();
    }
    for (int id : execution->targets.keys()) {
        const QString state = _executionStates.value(QString::number(id)).toString();
        if (execution->withdrawn.contains(id))
            emit executionProgress(execution->id, id, QStringLiteral("withdrawn"));
        else if (state == QStringLiteral("pending") || state == QStringLiteral("dispatching"))
            emit executionProgress(execution->id, id,
                                   success ? QStringLiteral("accepted") : QStringLiteral("cancelled"));
    }
    _executions.remove(execution->id);
    if (execution->context)
        execution->context->deleteLater();
    SwarmAuditLogger::instance()->logTaskEvent(
        QStringLiteral("execution_finished"),
        {{QStringLiteral("execution_id"), QVariant::fromValue(execution->id)},
         {QStringLiteral("success"), success},
         {QStringLiteral("reason"), reason},
         {QStringLiteral("reason_code"), execution->failureCode},
         {QStringLiteral("policy_revision"), QVariant::fromValue(execution->policyRevision)}});
    emit executionFinished(execution->id, success, reason);
}

void SwarmManager::_advanceExecution(const std::shared_ptr<Execution>& execution)
{
    if (execution->finished)
        return;
    if (_shutdownComplete || !_policyAllowed(execution)) {
        _finishExecution(execution, false, tr("自动执行许可已失效"));
        return;
    }
    if (execution->index >= execution->actions.size()) {
        _finishExecution(execution, true, {});
        return;
    }
    SwarmAction action = execution->actions[execution->index++];
    execution->activeAction = action;
    action.targetIds.removeIf([execution](int id) { return execution->withdrawn.contains(id); });
    if (action.targetIds.isEmpty()) {
        _continueExecution(execution);
        return;
    }
    QScopedValueRollback<bool> dispatchScope(_automationDispatchInProgress, true);
    QScopedValueRollback<std::optional<QList<int>>> targetsScope(_dispatchTargetIds, action.targetIds);
    const bool authorize = execution->approval == ExecutionApproval::OperatorConfirmed &&
                           (execution->origin != QStringLiteral("task") || _taskController->currentStageIndex() == 0);
    _requestAutomationLeases(authorize ? QStringLiteral("ai_confirmed") : QStringLiteral("ai"));
    if (planHasConstraintError({action})) {
        _finishExecution(execution, false, tr("实时约束已变化"));
        return;
    }
    for (int id : action.targetIds) {
        if (!_executionAllowed(execution, action, id)) {
            _finishExecution(execution, false, execution->failureReason);
            return;
        }
    }
    if (action.type == QStringLiteral("plan_coverage")) {
        if (_missionPlanner->busy()) {
            _finishExecution(execution, false, tr("任务上传忙碌"));
            return;
        }
        const quint64 batch = _missionPlanner->batchId() + 1;
        execution->plannerBatch = batch;
        connect(
            _missionPlanner, &SwarmMissionPlanner::batchFinished, execution->context,
            [this, execution, batch, ids = action.targetIds](quint64 completed, bool success, const QString& reason) {
                if (completed != batch || execution->finished)
                    return;
                if (!success)
                    _finishExecution(execution, false, reason);
                else {
                    for (int id : ids)
                        execution->plannedMissions[id] = _missionPlanner->verifiedSnapshot(id);
                    _continueExecution(execution);
                }
            });
        const auto& p = action.params;
        _missionPlanner->setSurveyAltitude(p.value(QStringLiteral("altitude")).toDouble());
        _missionPlanner->setSurveySpacing(p.value(QStringLiteral("spacing")).toDouble());
        if (!_missionPlanner->planAndUploadCoverage(
                QGeoCoordinate(p.value(QStringLiteral("center_lat")).toDouble(),
                               p.value(QStringLiteral("center_lon")).toDouble()),
                p.value(QStringLiteral("length")).toDouble(), p.value(QStringLiteral("width")).toDouble(),
                p.value(QStringLiteral("angle")).toDouble(),
                p.value(QStringLiteral("assign"), QStringLiteral("sequential")).toString(),
                [this, execution, action](int id) { return _executionAllowed(execution, action, id); }))
            _finishExecution(execution, false, tr("覆盖上传未受理"));
        return;
    }
    if (action.type == QStringLiteral("start_mission") || action.type == QStringLiteral("clear_missions")) {
        const bool clear = action.type == QStringLiteral("clear_missions");
        const quint64 batch = _missionPlanner->batchId() + 1;
        // A rejected busy batch must never become owned by this execution.
        if (_missionPlanner->busy()) {
            _finishExecution(execution, false, tr("任务读写忙碌，整批请求已拒绝"));
            return;
        }
        connect(_missionPlanner, &SwarmMissionPlanner::batchFinished, execution->context,
                [this, execution, action, clear, batch](quint64 completed, bool success, const QString& reason) {
                    if (completed != batch || execution->finished)
                        return;
                    execution->plannerBatch = 0;
                    if (!success) {
                        _finishExecution(execution, false, reason);
                        return;
                    }
                    if (clear) {
                        for (int id : action.targetIds)
                            if (!execution->withdrawn.contains(id))
                                emit executionProgress(execution->id, id, QStringLiteral("verified"));
                        _continueExecution(execution);
                        return;
                    }
                    for (int id : action.targetIds) {
                        if (execution->withdrawn.contains(id))
                            continue;
                        const auto snapshot = _missionPlanner->verifiedSnapshot(id);
                        if (snapshot.empty() || (execution->plannedMissions.contains(id) &&
                                                 !execution->plannedMissions.value(id).equivalent(snapshot))) {
                            execution->failureCode = QStringLiteral("mission_changed");
                            _finishExecution(execution, false, tr("机载任务为空或与已确认航线不一致，请重新规划上传"));
                            return;
                        }
                        execution->validatedMissions[id] = snapshot;
                        const auto target = execution->targets.value(id);
                        execution->contentRevisions[id] =
                            target && target->missionManager() ? target->missionManager()->contentRevision() : 0;
                    }
                    // Validate every bound target before sending the first mode/arm/start request.
                    for (int id : action.targetIds) {
                        if (!execution->withdrawn.contains(id) && !_executionAllowed(execution, action, id)) {
                            _finishExecution(execution, false, execution->failureReason);
                            return;
                        }
                    }
                    _dispatchExecutionAction(execution, action);
                });
        execution->plannerBatch = batch;
        if (!_missionPlanner->readMissionBatch(action.targetIds, clear, [this, execution, action](int id) {
                return _executionAllowed(execution, action, id);
            })) {
            execution->plannerBatch = 0;
            _finishExecution(execution, false, tr("任务管理器忙碌或目标许可无效，整批请求已拒绝"));
        }
        return;
    }
    _dispatchExecutionAction(execution, action);
}

void SwarmManager::_dispatchExecutionAction(const std::shared_ptr<Execution>& execution, SwarmAction action)
{
    if (execution->finished)
        return;
    action.targetIds.removeIf([execution](int id) { return execution->withdrawn.contains(id); });
    if (action.targetIds.isEmpty()) {
        _continueExecution(execution);
        return;
    }
    static const QSet<QString> commands{QStringLiteral("arm"),     QStringLiteral("disarm"),
                                        QStringLiteral("takeoff"), QStringLiteral("start_mission"),
                                        QStringLiteral("pause"),   QStringLiteral("rtl"),
                                        QStringLiteral("land"),    QStringLiteral("emergency_disarm")};
    const bool recovery = action.type == QStringLiteral("pause") || action.type == QStringLiteral("rtl") ||
                          action.type == QStringLiteral("land") || action.type == QStringLiteral("emergency_disarm");
    if (recovery) {
        // A task's own landing/return stage keeps its members in the task, but must end guidance.
        for (int id : action.targetIds)
            _formationTargetIds.remove(id);
        _formation->removeMembers(QSet<int>(action.targetIds.begin(), action.targetIds.end()));
    }
    if (!commands.contains(action.type)) {
        if (action.type == QStringLiteral("set_selection")) {
            QVariantList ids;
            for (int id : action.targetIds)
                ids.append(id);
            action.params[QStringLiteral("vehicle_ids")] = ids;
        }
        if (!_executeImmediateActions({action}, execution->origin, execution->approval)) {
            _finishExecution(execution, false, tr("动作被拒绝"));
            return;
        }
        _continueExecution(execution);
        return;
    }
    auto pending = std::make_shared<int>(action.targetIds.size());
    auto done = [this, execution, pending](int id, bool ok) {
        if (execution->finished)
            return;
        if (!ok && !execution->withdrawn.contains(id)) {
            emit executionProgress(execution->id, id, QStringLiteral("failed"));
            _finishExecution(execution, false,
                             execution->failureReason.isEmpty() ? tr("%1 号机动作失败；已执行成员保持当前任务").arg(id)
                                                                : execution->failureReason);
            return;
        }
        emit executionProgress(
            execution->id, id,
            execution->withdrawn.contains(id) ? QStringLiteral("withdrawn") : QStringLiteral("accepted"));
        if (--*pending == 0)
            _continueExecution(execution);
    };
    int delay = 0;
    for (int id : action.targetIds) {
        QPointer<Vehicle> target = execution->targets.value(id);
        auto allowed = [this, execution, action, id] { return _executionAllowed(execution, action, id); };
        QTimer::singleShot(delay, execution->context, [this, execution, action, target, id, allowed, done] {
            if (!allowed()) {
                done(id, false);
                return;
            }
            emit executionProgress(execution->id, id, QStringLiteral("dispatching"));
            if (!allowed()) {
                done(id, false);
                return;
            }
            SwarmAuditLogger::instance()->logActionDispatch(action.type, action.params, action.description);
            Vehicle* vehicle = target.data();
            if (!vehicle) {
                done(id, false);
                return;
            }
            const auto finished = [done, id](bool ok) { done(id, ok); };
            if (action.type == QStringLiteral("start_mission")) {
                if (!vehicle->missionManager() || vehicle->missionManager()->inProgress() ||
                    !execution->validatedMissions.contains(id)) {
                    done(id, false);
                    return;
                }
                vehicle->missionManager()->beginMissionExecution();
                vehicle->firmwarePlugin()->startMissionAsync(vehicle, execution->context, allowed, finished);
            } else if (action.type == QStringLiteral("takeoff")) {
                vehicle->firmwarePlugin()->takeoffAsync(vehicle,
                                                        action.params.value(QStringLiteral("altitude")).toDouble(),
                                                        execution->context, allowed, finished);
            } else if (action.type == QStringLiteral("arm") || action.type == QStringLiteral("disarm")) {
                const bool armed =
                    action.type == QStringLiteral("arm") && action.params.value(QStringLiteral("arm"), true).toBool();
                vehicle->setArmed(armed, true);
                auto* timer = new QTimer(execution->context);
                timer->setInterval(50);
                const qint64 started = SwarmClock::nowMs();
                connect(timer, &QTimer::timeout, timer, [target, allowed, finished, timer, started, armed] {
                    const bool reached = target && target->armed() == armed;
                    if (!allowed() || reached || SwarmClock::nowMs() - started > 10000) {
                        timer->stop();
                        timer->deleteLater();
                        finished(allowed() && reached);
                    }
                });
                timer->start();
            } else {
                if (action.type == QStringLiteral("pause"))
                    vehicle->pauseVehicle();
                else if (action.type == QStringLiteral("rtl"))
                    vehicle->guidedModeRTL(true);
                else if (action.type == QStringLiteral("land"))
                    vehicle->guidedModeLand();
                else
                    vehicle->sendMavCommand(vehicle->defaultComponentId(), MAV_CMD_COMPONENT_ARM_DISARM, true, 0,
                                            21196);
                done(id, true);
            }
            emit actionDispatched(execution->id, id, action.type);
        });
        if (action.type != QStringLiteral("pause") && action.type != QStringLiteral("emergency_disarm"))
            delay += action.type == QStringLiteral("takeoff") ? _takeoffStaggerMs : _commandIntervalMs;
    }
}

void SwarmManager::_continueExecution(const std::shared_ptr<Execution>& execution)
{
    execution->activeAction.reset();
    execution->validatedMissions.clear();
    execution->contentRevisions.clear();
    execution->plannerBatch = 0;
    if (!execution->finished)
        QTimer::singleShot(0, execution->context, [this, execution] { _advanceExecution(execution); });
}

bool SwarmManager::_policyAllowed(const std::shared_ptr<Execution>& execution) const
{
    if (execution->origin == QStringLiteral("operator") && execution->approval == ExecutionApproval::OperatorConfirmed)
        return true;
    if (aiExecutionBlocked())
        return false;
    auto remaining = execution->actions.mid(execution->index);
    if (execution->activeAction)
        remaining.prepend(*execution->activeAction);
    return execution->approval == ExecutionApproval::OperatorConfirmed || !needsConfirmation(remaining);
}

void SwarmManager::_enforcePolicy()
{
    const auto executions = _executions.values();
    for (const auto& execution : executions) {
        if (!_policyAllowed(execution)) {
            execution->failureCode = QStringLiteral("policy_revoked");
            _finishExecution(execution, false, tr("自主策略已收紧，未获许可的后续动作已取消"));
        }
    }
    SwarmAction formation;
    formation.type = QStringLiteral("start_formation");
    if (_formationAutomated && (aiExecutionBlocked() ||
                                (_formationApproval == ExecutionApproval::Automatic && needsConfirmation({formation}))))
        _formation->stop();
}

bool SwarmManager::_executionAllowed(const std::shared_ptr<Execution>& execution, const SwarmAction& action, int id)
{
    auto fail = [&](const QString& code, const QString& reason) {
        execution->failureCode = code;
        execution->failureReason = reason;
        return false;
    };
    if (execution->finished || _shutdownComplete || execution->withdrawn.contains(id))
        return fail(QStringLiteral("cancelled"), tr("执行已取消"));
    if (!_policyAllowed(execution))
        return fail(QStringLiteral("policy_revoked"), tr("当前自主策略要求重新确认"));
    const auto target = execution->targets.value(id);
    if (!target || target != MultiVehicleManager::instance()->getVehicleById(id) ||
        execution->generations.value(id) != _commandGenerations.value(id))
        return fail(QStringLiteral("target_changed"), tr("执行目标或指令代数已变化"));
    if (!_lease.canAutomate(id, SwarmClock::nowMs()))
        return fail(QStringLiteral("lease_revoked"), tr("控制租约已失效"));
    SwarmAction single = action;
    single.targetIds = {id};
    const auto violations = _planViolations({single}, 0, 0);
    for (const auto& violation : violations)
        if (violation.severity == QStringLiteral("error"))
            return fail(QStringLiteral("constraint_changed"), violation.message);
    if (action.type == QStringLiteral("start_mission") && execution->validatedMissions.contains(id)) {
        if (!target || !target->missionManager() || target->missionManager()->inProgress())
            return fail(QStringLiteral("mission_busy"), tr("航线核验后出现新的任务读写"));
        auto* mission = target->missionManager();
        if (execution->contentRevisions.value(id) != mission->contentRevision()) {
            const auto current = SwarmMissionSnapshot::capture(mission->missionItems(),
                                                               target->firmwarePlugin()->sendHomePositionToVehicle());
            if (!execution->validatedMissions.value(id).equivalent(current))
                return fail(QStringLiteral("mission_changed"), tr("已核验航线内容发生变化"));
            execution->contentRevisions[id] = mission->contentRevision();
        }
        const auto config = _constraintConfig();
        if (!target || execution->finished || !_policyAllowed(execution))
            return fail(QStringLiteral("cancelled"), tr("执行已取消"));
        const auto states = _vehicleStates(QList<int>{id});
        const auto reason = execution->validatedMissions.value(id).validate(
            target.data(), config, states.isEmpty() ? -1 : states.first().batteryPct);
        if (!reason.isEmpty())
            return fail(QStringLiteral("mission_constraint"), reason);
    }
    // Refreshing configuration can notify observers; recheck cancellation after those callbacks.
    return !execution->finished && !execution->withdrawn.contains(id) && target &&
           target == MultiVehicleManager::instance()->getVehicleById(id) &&
           execution->generations.value(id) == _commandGenerations.value(id) &&
           _lease.canAutomate(id, SwarmClock::nowMs()) && _policyAllowed(execution);
}
