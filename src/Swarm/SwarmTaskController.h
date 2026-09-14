/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmTaskController
 *
 * 集群任务状态机：多阶段任务编排。
 *   - 每个阶段 = 一组动作（SwarmAction，与人工/大模型同一管线）+ 一个完成条件；
 *   - 阶段进入时执行动作，随后 1 Hz 监控完成条件（基于各机遥测与 MISSION_CURRENT），
 *     条件满足自动推进下一阶段，直到任务完成；
 *   - 完成条件与任务来源无关：无论任务是 AI 生成还是人工在 Plan 视图设计并上传，
 *     监控当前任务版本的 MISSION_CURRENT 状态；不支持完成状态的固件需人工确认。
 *
 * 完成条件类型（complete_when.kind）：
 *   all_airborne         所有涉及机已解锁且相对高度 ≥ min_altitude（默认 3 米）
 *   all_disarmed         所有涉及机已上锁（降落完成）
 *   all_mission_finished 当前任务已启动并报告完成；未知完成状态须人工确认
 *   formation_running    编队控制器已激活且僚机全部入队
 *   duration             固定等待 seconds 秒
 *   manual               等待操作员点击“继续”（proceed()）
 *
 * 安全：阶段超时（默认 600 秒）判任务失败；abort() 中止任务并暂停任务所属成员。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtCore/QVariantMap>
#include <QtPositioning/QGeoCoordinate>
#include <QtQmlIntegration/QtQmlIntegration>

#include "SwarmAction.h"
#include "SwarmMissionSnapshot.h"

class SwarmManager;

class SwarmTaskController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("")

    Q_PROPERTY(bool running READ running NOTIFY taskChanged)
    Q_PROPERTY(bool blocked READ blocked NOTIFY taskChanged)
    Q_PROPERTY(QString taskName READ taskName NOTIFY taskChanged)
    Q_PROPERTY(int stageCount READ stageCount NOTIFY taskChanged)
    Q_PROPERTY(int currentStageIndex READ currentStageIndex NOTIFY taskChanged)
    Q_PROPERTY(QString currentStageName READ currentStageName NOTIFY taskChanged)
    Q_PROPERTY(QStringList stageNames READ stageNames NOTIFY taskChanged)
    Q_PROPERTY(QString stateText READ stateText NOTIFY taskChanged)
    Q_PROPERTY(bool waitingForProceed READ waitingForProceed NOTIFY taskChanged)
    Q_PROPERTY(bool canRetry READ canRetry NOTIFY taskChanged)
    Q_PROPERTY(bool awaitingMissionConfirmation READ awaitingMissionConfirmation NOTIFY taskChanged)
    Q_PROPERTY(QString executionToken READ executionToken NOTIFY taskChanged)
    Q_PROPERTY(QString taskId READ taskId NOTIFY taskChanged)
    Q_PROPERTY(Outcome outcome READ outcome NOTIFY taskChanged)
    Q_PROPERTY(QVariantMap result READ result NOTIFY taskChanged)
    Q_PROPERTY(QString persistenceError READ persistenceError NOTIFY taskChanged)

public:
    enum class Outcome
    {
        None,
        Succeeded,
        Aborted,
        Failed
    };
    Q_ENUM(Outcome)
    explicit SwarmTaskController(SwarmManager* swarmManager);
    ~SwarmTaskController();

    void shutdown();

    /// 一个任务阶段：进入时执行 actions，之后由状态机监控 complete_when 条件
    struct Stage
    {
        QString name;
        QList<SwarmAction> actions;
        QString conditionKind;        ///< all_airborne / all_disarmed / all_mission_finished /
                                      ///< formation_running / duration / manual
        QVariantMap conditionParams;  ///< min_altitude / seconds / timeout_sec / grace_sec
    };

    bool running() const { return _running; }

    bool blocked() const { return _blocked; }

    QString taskName() const { return _taskName; }

    int stageCount() const { return _stages.count(); }

    int currentStageIndex() const { return _currentStage; }

    QString currentStageName() const;
    QStringList stageNames() const;

    QString stateText() const { return _stateText; }

    bool waitingForProceed() const;

    /// 把阶段 JSON 数组解析校验为 Stage 列表（动作经 SwarmManager::parseAndValidate）。
    /// AI（SwarmAIAgent）与模板库（SwarmTaskLibrary）共用。
    QList<Stage> parseStages(const QJsonArray& stagesJson, const QGeoCoordinate& mapCenter,
                             QStringList& warnings) const;

    /// 启动多阶段任务（阶段与动作须先经 parseStages / SwarmManager::parseAndValidate 校验）
    bool startTask(const QString& taskName, const QList<Stage>& stages, bool operatorConfirmed = false);

    /// 中止任务：撤销任务所属成员的自动动作、编队引导并暂停这些成员
    Q_INVOKABLE void abort(const QString& reason = QString());

    /// 手动推进（conditionKind = manual 的阶段）
    Q_INVOKABLE void proceed();
    Q_INVOKABLE void retryBlockedStage();
    void withdrawTargets(const QList<int>& ids, const QString& reason);

    QList<int> activeTargets() const { return _running ? _taskTargetIds : QList<int>{}; }

    bool canRetry() const { return _blocked && !_stageDispatched; }

    bool awaitingMissionConfirmation() const { return _awaitingMissionConfirmation; }

    QString executionToken() const;

    QString taskId() const { return _taskId; }

    Outcome outcome() const { return _outcome; }

    QVariantMap result() const { return _result; }

    QString persistenceError() const { return _persistenceError; }

    QVariantList dispatchRecords() const { return _dispatchRecords; }

    QList<int> withdrawnTargets() const { return _withdrawnIds.values(); }

    Q_INVOKABLE bool confirmMissionCompletion(const QString& token);

#ifdef QGC_UNITTEST_BUILD
    /// 测试入口：读取已启动任务某阶段的超时值，用于校验编辑是否真的生效。
    int stageTimeoutSecForTest(int index) const
    {
        return (index >= 0 && index < _stages.count())
                   ? _stages.at(index).conditionParams.value(QStringLiteral("timeout_sec"), -1).toInt()
                   : -1;
    }
#endif

signals:
    void taskChanged();
    void taskFeedback(const QString& message);
    void taskFinished(bool success, const QString& message);
    void taskResultReady(const QString& taskId, const QVariantMap& result);

private slots:
    void _tick();

private:
    void _enterStage(int index);
    void _blockStage(const QString& reason);
    void _finish(Outcome outcome, const QString& message);
    bool _conditionMet(const Stage& stage);
    void _setStateText(const QString& text);
    void _persistState();

    SwarmManager* _swarmManager = nullptr;

    bool _running = false;
    bool _terminating = false;
    QString _taskId;
    Outcome _outcome = Outcome::None;
    QVariantMap _result;
    QVariantList _dispatchRecords;
    QVariantMap _withdrawalOffsets;
    QString _persistenceError;
    bool _blocked = false;
    bool _operatorConfirmed = false;
    QString _taskName;
    QList<Stage> _stages;
    int _currentStage = -1;
    QTimer _tickTimer;
    qint64 _stageStartMs = 0;
    QList<int> _involvedIds;    ///< 进入本阶段时快照的涉及机号
    QList<int> _taskTargetIds;  ///< 整项任务绑定且尚未退出的机号，用于授权续期与中止
    QSet<int> _withdrawnIds;
    quint64 _executionId = 0;
    quint64 _taskGeneration = 0;
    bool _actionsPending = false;
    bool _awaitingMissionConfirmation = false;
    bool _stageDispatched = false;
    QHash<int, quint64> _missionRevisions;
    QHash<int, SwarmMissionSnapshot> _plannedMissions;
    void _actionsFinished(quint64 id, bool success, const QString& reason);
    QSet<int> _vehiclesSeenInMission;  ///< all_mission_finished：已观察到进入 Mission 模式的机号
    QString _stateText;
};
