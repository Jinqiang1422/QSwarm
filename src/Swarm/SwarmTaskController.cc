#include "SwarmTaskController.h"

#include <QtCore/QDateTime>
#include <QtCore/QStandardPaths>
#include <QtCore/QUuid>
#include <cmath>

#include "Fact.h"
#include "FormationController.h"
#include "MissionManager.h"
#include "MultiVehicleManager.h"
#include "QGCApplication.h"
#include "QmlObjectListModel.h"
#include "SwarmAuditLogger.h"
#include "SwarmClock.h"
#include "SwarmManager.h"
#include "SwarmMissionPlanner.h"
#include "SwarmPersistence.h"
#include "Vehicle.h"
#include "VehicleFactGroup.h"

namespace {
constexpr int kDefaultTimeoutSec = 600;
constexpr int kDefaultGraceSec = 3;

QString taskPersistPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/SwarmConfig/tasks.json");
}
}  // namespace

SwarmTaskController::SwarmTaskController(SwarmManager* swarmManager)
    : QObject(swarmManager), _swarmManager(swarmManager)
{
    // Settle manager cancellation and withdrawal bookkeeping before task-stage continuation.
    connect(swarmManager, &SwarmManager::executionFinished, this, &SwarmTaskController::_actionsFinished,
            Qt::QueuedConnection);
    connect(swarmManager, &SwarmManager::actionDispatched, this,
            [this](quint64 executionId, int vehicleId, const QString& type) {
                if (_running && executionId == _executionId) {
                    _dispatchRecords.append(QVariantMap{{QStringLiteral("stage_index"), _currentStage},
                                                        {QStringLiteral("vehicle_id"), vehicleId},
                                                        {QStringLiteral("type"), type}});
                }
            });
    _tickTimer.setInterval(1000);
    (void) connect(&_tickTimer, &QTimer::timeout, this, &SwarmTaskController::_tick);
    (void) connect(this, &SwarmTaskController::taskFeedback, qgcApp(),
                   [](const QString& message) { qgcApp()->showAppMessage(message); });
    _setStateText(tr("空闲"));

    // 启动恢复：重启前未完成（RUNNING 已被 loadTasks 归一为 PAUSED）的任务不自动续飞，仅告警待确认
    bool recoveryPersisted = true;
    const auto restored = SwarmPersistence::loadTasks(taskPersistPath(), &recoveryPersisted);
    bool hasRecoveredTask = false;
    for (const auto& t : restored) {
        if (t.status == QStringLiteral("PAUSED") && !t.id.isEmpty()) {
            if (!hasRecoveredTask) {
                _taskName = t.id;
                _setStateText(tr("已暂停：等待操作员确认或重新启动"));
                hasRecoveredTask = true;
            }
            emit taskFeedback(tr("检测到重启前未完成任务「%1」，已暂停，请确认后重新启动").arg(t.id));
        }
    }
    if (!recoveryPersisted) {
        emit taskFeedback(tr("未能把恢复任务状态写回磁盘；请检查应用数据目录权限"));
    }
}

SwarmTaskController::~SwarmTaskController() = default;

void SwarmTaskController::shutdown()
{
    _tickTimer.stop();
    _running = false;
}

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
    for (const Stage& stage : _stages) {
        names.append(stage.name);
    }
    return names;
}

bool SwarmTaskController::waitingForProceed() const
{
    return _running && !_blocked && !_actionsPending && _currentStage >= 0 && _currentStage < _stages.count() &&
           _stages.at(_currentStage).conditionKind == QStringLiteral("manual");
}

// ---------------------------------------------------------------------------
// 阶段 JSON 解析校验（AI 与模板库共用；动作走统一管线）
// ---------------------------------------------------------------------------
QList<SwarmTaskController::Stage> SwarmTaskController::parseStages(const QJsonArray& stagesJson,
                                                                   const QGeoCoordinate& mapCenter,
                                                                   QStringList& warnings) const
{
    static const QSet<QString> kinds = {
        QStringLiteral("all_airborne"),      QStringLiteral("all_disarmed"), QStringLiteral("all_mission_finished"),
        QStringLiteral("formation_running"), QStringLiteral("duration"),     QStringLiteral("manual")};
    QList<Stage> stages;
    QList<int> targets;
    if (stagesJson.size() > 8) {
        warnings.append(tr("阶段数超过 8，整个任务已拒绝"));
        return {};
    }
    for (int i = 0; i < stagesJson.count(); i++) {  // 单个任务最多 8 个阶段
        const QJsonObject stageObj = stagesJson.at(i).toObject();
        Stage stage;
        stage.name = stageObj.value(QStringLiteral("name")).toString(tr("阶段 %1").arg(i + 1));

        const QJsonObject when = stageObj.value(QStringLiteral("complete_when")).toObject();
        stage.conditionKind = when.value(QStringLiteral("kind")).toString();
        if (!kinds.contains(stage.conditionKind)) {
            warnings.append(
                tr("阶段「%1」完成条件类型无效（%2），整个任务已拒绝").arg(stage.name, stage.conditionKind));
            return {};
        }
        for (const auto& key : {QStringLiteral("min_altitude"), QStringLiteral("seconds"),
                                QStringLiteral("timeout_sec"), QStringLiteral("grace_sec")}) {
            if (!when.contains(key))
                continue;
            const auto value = when.value(key);
            const double minimum = key == QStringLiteral("timeout_sec") ? 1.0 : 0.0;
            const double maximum = key == QStringLiteral("min_altitude") ? 120.0 : 86400.0;
            if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < minimum ||
                value.toDouble() > maximum ||
                (key != QStringLiteral("min_altitude") && value.toDouble() != value.toInt(-1))) {
                warnings.append(tr("阶段条件参数无效：%1").arg(key));
                return {};
            }
        }
        if (when.contains(QStringLiteral("grace_sec")))
            stage.conditionParams[QStringLiteral("grace_sec")] = when.value(QStringLiteral("grace_sec")).toInt();
        if (when.contains(QStringLiteral("min_altitude"))) {
            stage.conditionParams[QStringLiteral("min_altitude")] =
                when.value(QStringLiteral("min_altitude")).toDouble();
        }
        if (when.contains(QStringLiteral("seconds"))) {
            stage.conditionParams[QStringLiteral("seconds")] = when.value(QStringLiteral("seconds")).toInt();
        }
        if (when.contains(QStringLiteral("timeout_sec"))) {
            stage.conditionParams[QStringLiteral("timeout_sec")] = when.value(QStringLiteral("timeout_sec")).toInt();
        }
        if (stage.conditionKind == QStringLiteral("duration")) {
            const int seconds = stage.conditionParams.value(QStringLiteral("seconds"), 5).toInt();
            const int grace = stage.conditionParams.value(QStringLiteral("grace_sec"), 3).toInt();
            if (!stage.conditionParams.contains(QStringLiteral("timeout_sec")))
                stage.conditionParams[QStringLiteral("timeout_sec")] = qMax(600, seconds + grace + 30);
            if (stage.conditionParams.value(QStringLiteral("timeout_sec")).toInt() <= qMax(seconds, grace)) {
                warnings.append(tr("持续时间阶段的超时必须大于持续时间和宽限时间"));
                return {};
            }
        }

        const int warningsBefore = warnings.size();
        if (!stageObj.value(QStringLiteral("actions")).isArray()) {
            warnings.append(tr("阶段动作必须为数组"));
            return {};
        }
        stage.actions = _swarmManager->parseAndValidate(stageObj.value(QStringLiteral("actions")).toArray(), mapCenter,
                                                        warnings, targets);
        if (warnings.size() != warningsBefore)
            return {};
        if (stage.conditionKind == QStringLiteral("all_airborne")) {
            for (const auto& action : stage.actions) {
                if (action.type == QStringLiteral("takeoff") &&
                    stage.conditionParams.value(QStringLiteral("min_altitude"), 3).toDouble() >
                        action.params.value(QStringLiteral("altitude")).toDouble()) {
                    warnings.append(tr("升空判定高度不能高于起飞高度"));
                    return {};
                }
            }
        }
        if (!stage.actions.isEmpty())
            targets = stage.actions.last().targetIds;
        // duration / manual 阶段允许无动作（纯等待）；其余阶段必须有有效动作
        if (stage.actions.isEmpty() && stage.conditionKind != QStringLiteral("manual") &&
            stage.conditionKind != QStringLiteral("duration")) {
            warnings.append(tr("阶段「%1」无有效动作，整个任务已拒绝").arg(stage.name));
            return {};
        }
        stages.append(stage);
    }
    return stages;
}

bool SwarmTaskController::startTask(const QString& taskName, const QList<Stage>& stages, bool operatorConfirmed)
{
    if (_running || _terminating) {
        emit taskFeedback(tr("已有任务在运行，请先中止"));
        return false;
    }
    if (stages.isEmpty()) {
        emit taskFeedback(tr("任务启动失败：没有阶段"));
        return false;
    }
    for (const Stage& stage : stages) {
        static const QSet<QString> kinds = {
            QStringLiteral("all_airborne"),      QStringLiteral("all_disarmed"), QStringLiteral("all_mission_finished"),
            QStringLiteral("formation_running"), QStringLiteral("duration"),     QStringLiteral("manual")};
        if (!kinds.contains(stage.conditionKind)) {
            emit taskFeedback(tr("任务启动失败：阶段「%1」的完成条件类型无效").arg(stage.name));
            return false;
        }
    }

    ++_taskGeneration;
    const auto generation = _taskGeneration;
    _taskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    _outcome = Outcome::None;
    _result.clear();
    _dispatchRecords.clear();
    _withdrawalOffsets.clear();
    _persistenceError.clear();
    _operatorConfirmed = operatorConfirmed;
    _blocked = false;
    _involvedIds.clear();
    _taskTargetIds.clear();
    for (const auto& stage : stages)
        for (const auto& action : stage.actions)
            for (int id : action.targetIds)
                if (!_taskTargetIds.contains(id))
                    _taskTargetIds.append(id);
    _withdrawnIds.clear();
    _plannedMissions.clear();
    _taskName = taskName;
    _stages = stages;
    _running = true;
    _currentStage = -1;
    _stateText = tr("任务启动中");
    _persistState();
    emit taskChanged();
    if (!_running || generation != _taskGeneration)
        return true;
    emit taskFeedback(tr("集群任务「%1」启动，共 %2 个阶段").arg(taskName).arg(stages.count()));
    if (!_running || generation != _taskGeneration)
        return true;
    SwarmAuditLogger::instance()->logTaskEvent(
        QStringLiteral("task_started"),
        {{QStringLiteral("name"), taskName}, {QStringLiteral("stage_count"), stages.count()}});

    _enterStage(0);
    if (_running && generation == _taskGeneration && !_blocked)
        _tickTimer.start();
    return true;
}

void SwarmTaskController::abort(const QString& reason)
{
    if (!_running) {
        return;
    }
    SwarmAuditLogger::instance()->logTaskEvent(
        QStringLiteral("task_aborted"), {{QStringLiteral("name"), _taskName}, {QStringLiteral("reason"), reason}});
    _finish(Outcome::Aborted, reason.isEmpty() ? tr("任务被操作员中止") : reason);
}

void SwarmTaskController::proceed()
{
    if (waitingForProceed()) {
        const auto generation = _taskGeneration;
        const auto stage = _currentStage;
        emit taskFeedback(tr("阶段「%1」经操作员确认，继续").arg(currentStageName()));
        if (generation == _taskGeneration && stage == _currentStage && waitingForProceed())
            _enterStage(stage + 1);
    }
}

void SwarmTaskController::withdrawTargets(const QList<int>& ids, const QString& reason)
{
    if (!_running)
        return;
    for (int id : ids) {
        if (!_taskTargetIds.contains(id))
            continue;
        _withdrawnIds.insert(id);
        _withdrawalOffsets[QString::number(id)] = _dispatchRecords.size();
        _involvedIds.removeAll(id);
        _taskTargetIds.removeAll(id);
    }
    if (_taskTargetIds.isEmpty() && !_withdrawnIds.isEmpty()) {
        _finish(Outcome::Aborted, reason.isEmpty() ? tr("全部任务成员已退出") : reason);
    } else {
        _persistState();
        emit taskChanged();
    }
}

void SwarmTaskController::_blockStage(const QString& reason)
{
    _blocked = true;
    _tickTimer.stop();
    _setStateText(_stageDispatched ? tr("已阻塞：%1；需中止后重新启动").arg(reason)
                                   : tr("已阻塞：%1；修正后重试当前阶段").arg(reason));
    _persistState();
    emit taskChanged();
}

void SwarmTaskController::retryBlockedStage()
{
    if (!_running || !_blocked || _stageDispatched)
        return;
    _blocked = false;
    const auto generation = _taskGeneration;
    _enterStage(_currentStage);
    if (!_running || generation != _taskGeneration)
        return;
    if (!_blocked)
        _tickTimer.start();
    emit taskChanged();
}

void SwarmTaskController::_enterStage(int index)
{
    if (!_running || index < 0)
        return;
    const auto generation = _taskGeneration;
    const auto current = [this, generation, index] {
        return _running && generation == _taskGeneration && index == _currentStage;
    };
    if (index >= _stages.count()) {
        _finish(Outcome::Succeeded, tr("集群任务「%1」全部 %2 个阶段已完成").arg(_taskName).arg(_stages.count()));
        return;
    }

    _currentStage = index;
    _stageStartMs = SwarmClock::nowMs();
    _vehiclesSeenInMission.clear();
    _missionRevisions.clear();
    _awaitingMissionConfirmation = false;
    _stageDispatched = false;
    _actionsPending = false;

    Stage stage = _stages.at(index);
    for (auto& action : stage.actions) {
        action.targetIds.removeIf([this](int id) { return _withdrawnIds.contains(id); });
        if (action.type == QStringLiteral("set_selection")) {
            QVariantList ids;
            for (int id : action.targetIds)
                ids.append(id);
            action.params[QStringLiteral("vehicle_ids")] = ids;
        }
        if (action.type == QStringLiteral("start_formation") &&
            _withdrawnIds.contains(action.params.value(QStringLiteral("leader_id")).toInt())) {
            const int currentLeader = _swarmManager->formation()->leaderVehicleId();
            action.params[QStringLiteral("leader_id")] =
                action.targetIds.contains(currentLeader) ? currentLeader
                                                         : (action.targetIds.isEmpty() ? 0 : action.targetIds.first());
        }
    }
    stage.actions.removeIf([](const SwarmAction& action) { return action.targetIds.isEmpty(); });
    if (stage.actions.isEmpty() && !_stages.at(index).actions.isEmpty()) {
        _enterStage(index + 1);
        return;
    }
    if (!stage.actions.isEmpty()) {
        _involvedIds.clear();
        for (const auto& action : stage.actions) {
            for (int id : action.targetIds) {
                if (!_involvedIds.contains(id))
                    _involvedIds.append(id);
                if (!_taskTargetIds.contains(id))
                    _taskTargetIds.append(id);
            }
        }
    }
    emit taskFeedback(tr("阶段 %1/%2「%3」开始，执行 %4 项动作")
                          .arg(index + 1)
                          .arg(_stages.count())
                          .arg(stage.name)
                          .arg(stage.actions.count()));
    if (!current())
        return;
    SwarmAuditLogger::instance()->logTaskEvent(QStringLiteral("stage_entered"),
                                               {{QStringLiteral("index"), index + 1},
                                                {QStringLiteral("name"), stage.name},
                                                {QStringLiteral("action_count"), stage.actions.count()}});

    // 推进前用实时机况重校验约束:运行中电量下降/机离线导致 error 则暂停(而非盲目下发)
    const bool constraintError = _swarmManager->planHasConstraintError(stage.actions);
    if (!current())
        return;
    if (constraintError) {
        emit taskFeedback(tr("阶段 %1/%2「%3」推进受阻：实时约束校验发现 error（如电量不足），任务暂停")
                              .arg(index + 1)
                              .arg(_stages.count())
                              .arg(stage.name));
        if (!current())
            return;
        SwarmAuditLogger::instance()->logTaskEvent(
            QStringLiteral("stage_blocked_by_constraint"),
            {{QStringLiteral("index"), index + 1}, {QStringLiteral("name"), stage.name}});
        _blockStage(tr("阶段 %1 实时约束校验未通过").arg(index + 1));
        return;
    }

    // 执行本阶段动作（统一管线）
    if (index == 0 && _operatorConfirmed)
        _swarmManager->authorizeTaskTargets(_taskTargetIds);
    _actionsPending = true;
    _executionId = 0;
    const bool accepted =
        _swarmManager->executeActions(stage.actions, QStringLiteral("task"),
                                      _operatorConfirmed ? SwarmManager::ExecutionApproval::OperatorConfirmed
                                                         : SwarmManager::ExecutionApproval::Automatic,
                                      _plannedMissions, &_executionId);
    if (!current())
        return;
    if (!accepted) {
        _actionsPending = false;
        _blockStage(tr("动作执行被拒绝"));
        return;
    }

    if (!_actionsPending)
        return;  // A synchronous cancellation already settled this execution.
    _stageDispatched = true;
    _setStateText(tr("执行阶段动作中：%1").arg(stage.name));

    _persistState();
    emit taskChanged();
}

void SwarmTaskController::_tick()
{
    if (!_running || _blocked || _actionsPending || _currentStage < 0 || _currentStage >= _stages.count()) {
        return;
    }

    const Stage stage = _stages.at(_currentStage);
    const auto generation = _taskGeneration;
    const auto index = _currentStage;
    const qint64 elapsedMs = SwarmClock::nowMs() - _stageStartMs;
    const int timeoutSec = stage.conditionParams.value(QStringLiteral("timeout_sec"), kDefaultTimeoutSec).toInt();
    const int graceSec = stage.conditionParams.value(QStringLiteral("grace_sec"), kDefaultGraceSec).toInt();

    if (_involvedIds.isEmpty() && !_withdrawnIds.isEmpty()) {
        _enterStage(_currentStage + 1);
        return;
    }
    if (stage.conditionKind == QStringLiteral("manual"))
        return;

    if (!_awaitingMissionConfirmation && elapsedMs > timeoutSec * 1000) {
        SwarmAuditLogger::instance()->logTaskEvent(
            QStringLiteral("stage_timeout"),
            {{QStringLiteral("name"), stage.name}, {QStringLiteral("timeout_sec"), timeoutSec}});
        _finish(Outcome::Failed, tr("阶段「%1」超过 %2 秒未完成").arg(stage.name).arg(timeoutSec));
        return;
    }

    // 宽限期内不判定条件（等待各机响应动作），manual 阶段仅靠 proceed() 推进
    if (elapsedMs < graceSec * 1000 || stage.conditionKind == QStringLiteral("manual")) {
        return;
    }

    if (_conditionMet(stage)) {
        if (!_running || generation != _taskGeneration || index != _currentStage)
            return;
        emit taskFeedback(tr("阶段「%1」完成条件满足，自动推进").arg(stage.name));
        if (!_running || generation != _taskGeneration || index != _currentStage)
            return;
        SwarmAuditLogger::instance()->logTaskEvent(
            QStringLiteral("stage_completed"),
            {{QStringLiteral("name"), stage.name}, {QStringLiteral("elapsed_ms"), QVariant::fromValue(elapsedMs)}});
        _enterStage(_currentStage + 1);
    }
}

bool SwarmTaskController::_conditionMet(const Stage& stage)
{
    MultiVehicleManager* mvm = MultiVehicleManager::instance();
    const QString kind = stage.conditionKind;

    if (kind == QStringLiteral("formation_running")) {
        FormationController* formation = _swarmManager->formation();
        return formation->active() && !formation->blocked() &&
               formation->followerCount() >= qMax(0, _involvedIds.count() - 1);
    }
    if (kind == QStringLiteral("duration")) {
        const int seconds = stage.conditionParams.value(QStringLiteral("seconds"), 5).toInt();
        return (SwarmClock::nowMs() - _stageStartMs) >= seconds * 1000;
    }

    // 基于各机状态的条件：逐机检查，任何涉及机离线视为任务异常
    int finishedCount = 0;
    const auto involvedIds = _involvedIds;
    const auto generation = _taskGeneration;
    const auto stageIndex = _currentStage;
    for (const int id : involvedIds) {
        Vehicle* vehicle = mvm->getVehicleById(id);
        if (!vehicle) {
            return false;  // 有涉及机离线，等待超时机制处理
        }
        VehicleFactGroup* vfg = qobject_cast<VehicleFactGroup*>(vehicle->vehicleFactGroup());
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
            MissionManager* mission = vehicle->missionManager();
            if (!mission || mission->executionRevision() != _missionRevisions.value(id)) {
                _blockStage(tr("机载任务版本变化，需要重新启动任务"));
                return false;
            }
            if (mission->missionState() == MISSION_STATE_PAUSED ||
                mission->missionState() == MISSION_STATE_NO_MISSION) {
                _blockStage(tr("飞控报告任务暂停或不存在，需要操作员处置"));
                return false;
            }
            if (vehicle->armed() && vehicle->flightMode() == vehicle->missionFlightMode())
                _vehiclesSeenInMission.insert(id);
            if (mission->executionComplete() && _vehiclesSeenInMission.contains(id)) {
                ++finishedCount;
            } else if (_vehiclesSeenInMission.contains(id) && mission->missionState() == MISSION_STATE_UNKNOWN) {
                _awaitingMissionConfirmation = true;
                _setStateText(tr("固件未提供可靠完成状态；请核对各机任务后人工确认"));
                emit taskChanged();
                if (!_running || generation != _taskGeneration || stageIndex != _currentStage)
                    return false;
            }
        }
    }
    return finishedCount == _involvedIds.count() && !_involvedIds.isEmpty();
}

void SwarmTaskController::_finish(Outcome outcome, const QString& message)
{
    if (!_running || _terminating)
        return;
    _terminating = true;
    _tickTimer.stop();
    _running = false;
    _blocked = false;
    _actionsPending = false;
    _awaitingMissionConfirmation = false;
    _outcome = outcome;
    const bool success = outcome == Outcome::Succeeded;
    if (!success) {
        const auto targets = _taskTargetIds;
        _swarmManager->cancelTargets(targets, message);
        _swarmManager->pauseTargets(targets);
    }
    _setStateText(
        success ? (_withdrawnIds.isEmpty() ? tr("已完成") : tr("剩余成员已完成，%1 架退出").arg(_withdrawnIds.size()))
                : tr("已中止/失败"));
    QVariantList completed, withdrawn;
    if (success)
        for (int id : _taskTargetIds)
            completed.append(id);
    for (int id : _withdrawnIds)
        withdrawn.append(id);
    const QString outcomeName = success                       ? QStringLiteral("Succeeded")
                                : outcome == Outcome::Aborted ? QStringLiteral("Aborted")
                                                              : QStringLiteral("Failed");
    _result = {{QStringLiteral("task_id"), _taskId},
               {QStringLiteral("outcome"), outcomeName},
               {QStringLiteral("reason"), message},
               {QStringLiteral("completed_ids"), completed},
               {QStringLiteral("withdrawn_ids"), withdrawn},
               {QStringLiteral("dispatches"), _dispatchRecords},
               {QStringLiteral("withdrawal_offsets"), _withdrawalOffsets}};
    _persistenceError =
        SwarmPersistence::saveTasks(taskPersistPath(), {}) ? QString() : tr("任务终态保存失败，请检查数据目录");
    const auto resultSnapshot = _result;
    const auto finishedId = _taskId;
    _terminating = false;
    // No state writes after the first notification: listeners may synchronously start a new task.
    emit taskChanged();
    emit taskResultReady(finishedId, resultSnapshot);
    emit taskFeedback(message);
    emit taskFinished(success, message);
}

void SwarmTaskController::_persistState()
{
    SwarmPersistence::TaskRecord r;
    r.id = _taskName;
    r.type = QStringLiteral("MULTISTAGE");
    r.status = (running() && !_blocked) ? QStringLiteral("RUNNING") : QStringLiteral("PAUSED");
    r.version = 1;
    _persistenceError =
        SwarmPersistence::saveTasks(taskPersistPath(), {r}) ? QString() : tr("任务状态保存失败，请检查数据目录");
}

void SwarmTaskController::_setStateText(const QString& text)
{
    _stateText = text;
}

void SwarmTaskController::_actionsFinished(quint64 id, bool success, const QString& reason)
{
    if (!_running || !_actionsPending || id != _executionId)
        return;
    _actionsPending = false;
    if (!success) {
        if (_involvedIds.isEmpty() && !_withdrawnIds.isEmpty()) {
            _enterStage(_currentStage + 1);
            return;
        }
        _blockStage(tr("动作部分执行或失败：%1；需中止后重新启动").arg(reason));
        return;
    }
    _stageStartMs = SwarmClock::nowMs();
    for (const auto& action : _stages.at(_currentStage).actions) {
        if (action.type == QStringLiteral("plan_coverage")) {
            for (int vehicleId : action.targetIds) {
                if (!_withdrawnIds.contains(vehicleId))
                    _plannedMissions[vehicleId] = _swarmManager->missionPlanner()->verifiedSnapshot(vehicleId);
            }
        }
    }
    _setStateText(waitingForProceed() ? tr("等待操作员确认：%1").arg(currentStageName())
                                      : tr("等待阶段完成条件：%1").arg(currentStageName()));
    for (int vehicleId : _involvedIds) {
        Vehicle* vehicle = MultiVehicleManager::instance()->getVehicleById(vehicleId);
        if (!vehicle || !vehicle->missionManager())
            continue;
        _missionRevisions[vehicleId] = vehicle->missionManager()->executionRevision();
        if (vehicle->armed() && vehicle->flightMode() == vehicle->missionFlightMode())
            _vehiclesSeenInMission.insert(vehicleId);
    }
    emit taskChanged();
}

QString SwarmTaskController::executionToken() const
{
    return QStringLiteral("%1:%2:%3").arg(_taskGeneration).arg(_currentStage).arg(_executionId);
}

bool SwarmTaskController::confirmMissionCompletion(const QString& token)
{
    if (!_running || _blocked || !_awaitingMissionConfirmation || token != executionToken())
        return false;
    for (int id : _involvedIds) {
        Vehicle* vehicle = MultiVehicleManager::instance()->getVehicleById(id);
        if (!vehicle || !vehicle->missionManager() || !_vehiclesSeenInMission.contains(id) ||
            vehicle->missionManager()->executionRevision() != _missionRevisions.value(id))
            return false;
        const int state = vehicle->missionManager()->missionState();
        if (state != MISSION_STATE_UNKNOWN && state != MISSION_STATE_COMPLETE)
            return false;
    }
    SwarmAuditLogger::instance()->logTaskEvent(
        QStringLiteral("mission_completion_operator_confirmed"),
        {{QStringLiteral("token"), token}, {QStringLiteral("stage"), currentStageName()}});
    _awaitingMissionConfirmation = false;
    _enterStage(_currentStage + 1);
    return true;
}
