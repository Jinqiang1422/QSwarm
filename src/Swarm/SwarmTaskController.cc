#include "SwarmTaskController.h"

#include "SwarmManager.h"
#include "SwarmAuditLogger.h"
#include "FormationController.h"
#include "MultiVehicleManager.h"
#include "Vehicle.h"
#include "VehicleFactGroup.h"
#include "Fact.h"
#include "QmlObjectListModel.h"
#include "QGCApplication.h"
#include "SwarmPersistence.h"

#include <QtCore/QDateTime>
#include <QtCore/QStandardPaths>

namespace {
    constexpr int kDefaultTimeoutSec = 600;
    constexpr int kDefaultGraceSec   = 3;

    QString taskPersistPath()
    {
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
             + QStringLiteral("/SwarmConfig/tasks.json");
    }
}

SwarmTaskController::SwarmTaskController(SwarmManager *swarmManager)
    : QObject(swarmManager)
    , _swarmManager(swarmManager)
{
    _tickTimer.setInterval(1000);
    (void) connect(&_tickTimer, &QTimer::timeout, this, &SwarmTaskController::_tick);
    (void) connect(this, &SwarmTaskController::taskFeedback, qgcApp(), [](const QString &message) {
        qgcApp()->showAppMessage(message);
    });
    _setStateText(tr("空闲"));

    // 启动恢复：重启前未完成（RUNNING 已被 loadTasks 归一为 PAUSED）的任务不自动续飞，仅告警待确认
    const auto restored = SwarmPersistence::loadTasks(taskPersistPath());
    for (const auto &t : restored) {
        if (t.status == QStringLiteral("PAUSED") && !t.id.isEmpty()) {
            emit taskFeedback(tr("检测到重启前未完成任务「%1」，已暂停，请确认后重新启动").arg(t.id));
        }
    }
}

SwarmTaskController::~SwarmTaskController() = default;

QString SwarmTaskController::currentStageName() const
{
    if (_currentStage >= 0 && _currentStage < _stages.count()) {
        return _stages.at(_currentStage).name;
    }
    return QString();
}

QStringList SwarmTaskController::stageNames() const
{
    QStringList names;
    for (const Stage &stage : _stages) {
        names.append(stage.name);
    }
    return names;
}

bool SwarmTaskController::waitingForProceed() const
{
    return _running && _currentStage >= 0 && _currentStage < _stages.count()
           && _stages.at(_currentStage).conditionKind == QStringLiteral("manual");
}

// ---------------------------------------------------------------------------
// 阶段 JSON 解析校验（AI 与模板库共用；动作走统一管线）
// ---------------------------------------------------------------------------
QList<SwarmTaskController::Stage> SwarmTaskController::parseStages(const QJsonArray &stagesJson,
                                                                   const QGeoCoordinate &mapCenter,
                                                                   QStringList &warnings) const
{
    static const QSet<QString> kinds = {
        QStringLiteral("all_airborne"), QStringLiteral("all_disarmed"), QStringLiteral("all_mission_finished"),
        QStringLiteral("formation_running"), QStringLiteral("duration"), QStringLiteral("manual")
    };
    QList<Stage> stages;

    for (int i = 0; i < qMin(stagesJson.count(), 8); i++) {     // 单个任务最多 8 个阶段
        const QJsonObject stageObj = stagesJson.at(i).toObject();
        Stage stage;
        stage.name = stageObj.value(QStringLiteral("name")).toString(tr("阶段 %1").arg(i + 1));

        const QJsonObject when = stageObj.value(QStringLiteral("complete_when")).toObject();
        stage.conditionKind = when.value(QStringLiteral("kind")).toString();
        if (!kinds.contains(stage.conditionKind)) {
            warnings.append(tr("阶段「%1」完成条件类型无效（%2），该阶段已忽略").arg(stage.name, stage.conditionKind));
            continue;
        }
        if (when.contains(QStringLiteral("min_altitude"))) {
            stage.conditionParams[QStringLiteral("min_altitude")] = when.value(QStringLiteral("min_altitude")).toDouble();
        }
        if (when.contains(QStringLiteral("seconds"))) {
            stage.conditionParams[QStringLiteral("seconds")] = when.value(QStringLiteral("seconds")).toInt();
        }
        if (when.contains(QStringLiteral("timeout_sec"))) {
            stage.conditionParams[QStringLiteral("timeout_sec")] = when.value(QStringLiteral("timeout_sec")).toInt();
        }

        stage.actions = _swarmManager->parseAndValidate(
                    stageObj.value(QStringLiteral("actions")).toArray(), mapCenter, warnings);
        // duration / manual 阶段允许无动作（纯等待）；其余阶段必须有有效动作
        if (stage.actions.isEmpty()
                && stage.conditionKind != QStringLiteral("manual")
                && stage.conditionKind != QStringLiteral("duration")) {
            warnings.append(tr("阶段「%1」无有效动作，已忽略").arg(stage.name));
            continue;
        }
        stages.append(stage);
    }
    return stages;
}

bool SwarmTaskController::startTask(const QString &taskName, const QList<Stage> &stages)
{
    if (_running) {
        emit taskFeedback(tr("已有任务在运行，请先中止"));
        return false;
    }
    if (stages.isEmpty()) {
        emit taskFeedback(tr("任务启动失败：没有阶段"));
        return false;
    }
    for (const Stage &stage : stages) {
        static const QSet<QString> kinds = {
            QStringLiteral("all_airborne"), QStringLiteral("all_disarmed"), QStringLiteral("all_mission_finished"),
            QStringLiteral("formation_running"), QStringLiteral("duration"), QStringLiteral("manual")
        };
        if (!kinds.contains(stage.conditionKind)) {
            emit taskFeedback(tr("任务启动失败：阶段「%1」的完成条件类型无效").arg(stage.name));
            return false;
        }
    }

    _taskName     = taskName;
    _stages       = stages;
    _running      = true;
    _currentStage = -1;
    emit taskChanged();
    emit taskFeedback(tr("集群任务「%1」启动，共 %2 个阶段").arg(taskName).arg(stages.count()));
    SwarmAuditLogger::instance()->logTaskEvent(QStringLiteral("task_started"), {
        {QStringLiteral("name"),        taskName},
        {QStringLiteral("stage_count"), stages.count()}
    });

    _enterStage(0);
    _tickTimer.start();
    _persistState();
    return true;
}

void SwarmTaskController::abort(const QString &reason)
{
    if (!_running) {
        return;
    }
    SwarmAuditLogger::instance()->logTaskEvent(QStringLiteral("task_aborted"), {
        {QStringLiteral("name"),   _taskName},
        {QStringLiteral("reason"), reason}
    });
    _swarmManager->pauseAll();      // 安全：中止时全体暂停悬停，由操作员接管
    _finish(false, reason.isEmpty() ? tr("任务被操作员中止") : reason);
}

void SwarmTaskController::proceed()
{
    if (waitingForProceed()) {
        emit taskFeedback(tr("阶段「%1」经操作员确认，继续").arg(currentStageName()));
        _enterStage(_currentStage + 1);
    }
}

void SwarmTaskController::_enterStage(int index)
{
    if (index >= _stages.count()) {
        _finish(true, tr("集群任务「%1」全部 %2 个阶段已完成").arg(_taskName).arg(_stages.count()));
        return;
    }

    _currentStage = index;
    _stageStartMs = QDateTime::currentMSecsSinceEpoch();
    _vehiclesSeenInMission.clear();

    const Stage &stage = _stages.at(index);
    emit taskFeedback(tr("阶段 %1/%2「%3」开始，执行 %4 项动作")
                          .arg(index + 1).arg(_stages.count()).arg(stage.name).arg(stage.actions.count()));
    SwarmAuditLogger::instance()->logTaskEvent(QStringLiteral("stage_entered"), {
        {QStringLiteral("index"),        index + 1},
        {QStringLiteral("name"),         stage.name},
        {QStringLiteral("action_count"), stage.actions.count()}
    });

    // 推进前用实时机况重校验约束:运行中电量下降/机离线导致 error 则暂停(而非盲目下发)
    if (_swarmManager->planHasConstraintError(stage.actions)) {
        emit taskFeedback(tr("阶段 %1/%2「%3」推进受阻：实时约束校验发现 error（如电量不足），任务暂停")
                              .arg(index + 1).arg(_stages.count()).arg(stage.name));
        SwarmAuditLogger::instance()->logTaskEvent(QStringLiteral("stage_blocked_by_constraint"), {
            {QStringLiteral("index"), index + 1}, {QStringLiteral("name"), stage.name}
        });
        _setStateText(tr("已暂停：阶段 %1 实时约束校验未通过").arg(index + 1));
        return;
    }

    // 执行本阶段动作（统一管线）
    _swarmManager->executeActions(stage.actions, QStringLiteral("task"));

    if (stage.conditionKind == QStringLiteral("manual")) {
        _setStateText(tr("等待操作员确认：阶段 %1/%2「%3」").arg(index + 1).arg(_stages.count()).arg(stage.name));
    } else {
        _setStateText(tr("运行中：阶段 %1/%2「%3」").arg(index + 1).arg(_stages.count()).arg(stage.name));
    }

    // 动作可能改变选择集（set_selection），动作执行后快照涉及机号用于条件监控
    MultiVehicleManager *mvm = MultiVehicleManager::instance();
    QmlObjectListModel *model = (mvm->selectedVehicles()->count() > 0) ? mvm->selectedVehicles()
                                                                       : mvm->vehicles();
    _involvedIds.clear();
    for (int i = 0; i < model->count(); i++) {
        Vehicle *vehicle = qobject_cast<Vehicle *>(model->get(i));
        if (vehicle) {
            _involvedIds.append(vehicle->id());
        }
    }
    emit taskChanged();
    _persistState();
}

void SwarmTaskController::_tick()
{
    if (!_running || _currentStage < 0 || _currentStage >= _stages.count()) {
        return;
    }

    const Stage &stage = _stages.at(_currentStage);
    const qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - _stageStartMs;
    const int timeoutSec = stage.conditionParams.value(QStringLiteral("timeout_sec"), kDefaultTimeoutSec).toInt();
    const int graceSec   = stage.conditionParams.value(QStringLiteral("grace_sec"), kDefaultGraceSec).toInt();

    if (elapsedMs > timeoutSec * 1000) {
        SwarmAuditLogger::instance()->logTaskEvent(QStringLiteral("stage_timeout"), {
            {QStringLiteral("name"),        stage.name},
            {QStringLiteral("timeout_sec"), timeoutSec}
        });
        _finish(false, tr("阶段「%1」超过 %2 秒未完成，任务失败，全体已暂停悬停").arg(stage.name).arg(timeoutSec));
        _swarmManager->pauseAll();
        return;
    }

    // 宽限期内不判定条件（等待各机响应动作），manual 阶段仅靠 proceed() 推进
    if (elapsedMs < graceSec * 1000 || stage.conditionKind == QStringLiteral("manual")) {
        return;
    }

    if (_conditionMet(stage)) {
        emit taskFeedback(tr("阶段「%1」完成条件满足，自动推进").arg(stage.name));
        SwarmAuditLogger::instance()->logTaskEvent(QStringLiteral("stage_completed"), {
            {QStringLiteral("name"),       stage.name},
            {QStringLiteral("elapsed_ms"), QVariant::fromValue(elapsedMs)}
        });
        _enterStage(_currentStage + 1);
    }
}

bool SwarmTaskController::_conditionMet(const Stage &stage)
{
    MultiVehicleManager *mvm = MultiVehicleManager::instance();
    const QString kind = stage.conditionKind;

    if (kind == QStringLiteral("formation_running")) {
        FormationController *formation = _swarmManager->formation();
        return formation->active() && formation->followerCount() >= qMax(0, _involvedIds.count() - 1);
    }
    if (kind == QStringLiteral("duration")) {
        const int seconds = stage.conditionParams.value(QStringLiteral("seconds"), 5).toInt();
        return (QDateTime::currentMSecsSinceEpoch() - _stageStartMs) >= seconds * 1000;
    }

    // 基于各机状态的条件：逐机检查，任何涉及机离线视为任务异常
    int finishedCount = 0;
    for (const int id : _involvedIds) {
        Vehicle *vehicle = mvm->getVehicleById(id);
        if (!vehicle) {
            return false;   // 有涉及机离线，等待超时机制处理
        }
        VehicleFactGroup *vfg = qobject_cast<VehicleFactGroup *>(vehicle->vehicleFactGroup());
        const double altRel = vfg ? vfg->altitudeRelative()->rawValue().toDouble() : 0.0;

        if (kind == QStringLiteral("all_airborne")) {
            const double minAlt = stage.conditionParams.value(QStringLiteral("min_altitude"), 3.0).toDouble();
            if (vehicle->armed() && altRel >= minAlt - 0.5) {
                finishedCount++;
            }
        } else if (kind == QStringLiteral("all_disarmed")) {
            if (!vehicle->armed()) {
                finishedCount++;
            }
        } else if (kind == QStringLiteral("all_mission_finished")) {
            if (!vehicle->armed()) {
                finishedCount++;    // 已降落上锁（如任务以 RTL 结束）
                continue;
            }
            // 监控 MISSION_CURRENT 驱动的模式变化：曾进入 Mission 模式后退出即视为完成。
            // 与任务来源无关——AI 上传与人工 Plan 视图上传的任务均由此判定。
            const QString missionMode = vehicle->missionFlightMode();
            if (vehicle->flightMode() == missionMode) {
                _vehiclesSeenInMission.insert(id);
            } else if (_vehiclesSeenInMission.contains(id)) {
                finishedCount++;
            }
        }
    }
    return finishedCount == _involvedIds.count() && !_involvedIds.isEmpty();
}

void SwarmTaskController::_finish(bool success, const QString &message)
{
    _tickTimer.stop();
    _running = false;
    emit taskChanged();
    _setStateText(success ? tr("已完成") : tr("已中止/失败"));
    emit taskFeedback(message);
    emit taskFinished(success, message);
    _persistState();
}

void SwarmTaskController::_persistState()
{
    SwarmPersistence::TaskRecord r;
    r.id      = _taskName;
    r.type    = QStringLiteral("MULTISTAGE");
    r.status  = running() ? QStringLiteral("RUNNING") : QStringLiteral("PAUSED");
    r.version = 1;
    SwarmPersistence::saveTasks(taskPersistPath(), { r });
}

void SwarmTaskController::_setStateText(const QString &text)
{
    if (_stateText != text) {
        _stateText = text;
        emit taskChanged();
    }
}
