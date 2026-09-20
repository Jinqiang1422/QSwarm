#include "SwarmDispatchLockTest.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>

#include "MockLink.h"
#include "SwarmAIAgent.h"
#include "SwarmManager.h"
#include "SwarmTaskController.h"
#include "Vehicle.h"

SwarmDispatchLockTest::~SwarmDispatchLockTest() = default;

void SwarmDispatchLockTest::init()
{
    VehicleTest::init();
    // MockLink does not serve COMP_METADATA_TYPE_GENERAL; the primary/fallback download
    // failure is environmental (this host has restricted network access), not a defect
    // under test. Whether the attempt fails inside this test's strict-log capture window
    // is timing-dependent, which otherwise makes an unrelated test flake. See
    // QmlUITestBase::ignoreAPMMockLinkWarnings for the identical precedent.
    ignoreLogMessage("ComponentInformation.RequestMetaDataTypeStateMachine", QtWarningMsg,
                     QRegularExpression(QStringLiteral(
                         "\"COMP_METADATA_TYPE_GENERAL\" : failed to load metadata \\(primary and fallback\\) \"\"")));
    QVERIFY(vehicle());
    ignoreLogMessage(
        "API.QGCApplication.AppMessage", QtDebugMsg,
        QRegularExpression(
            "(集群任务|阶段 |阶段「|检测到重启前未完成任务|区域覆盖任务已提交上传请求|已请求清空|[0-9]+ 号机：)"));
    // 锁定构建只验证 aiAutodispatchAvailable()/deliveryNotice() 这两个只读属性，不调用
    // setAutonomyLevel(2)：锁定构建会在任务 10 里把自主等级钉死在 L2，若这里手动设成
    // 2，即便钉死逻辑没生效，测试也会碰巧通过，证明不了任何东西。
    _manager = std::make_unique<SwarmManager>();
    // 三条下发用例都靠 pause 动作触发的 MAV_CMD_DO_REPOSITION 计数区分“下发了没有”；
    // 不清零就会把上一条用例（或 SwarmManager 构造期间的探测流量）的计数带进来，
    // 让本该等于 0 的断言碰巧通过。
    mockLink()->clearReceivedMavCommandCounts();
}

void SwarmDispatchLockTest::cleanup()
{
    if (_manager) {
        // 若用例意外启动了任务状态机，不主动中止就直接 shutdown()，_persistState 落盘的
        // RUNNING 记录会在下一条用例构造 SwarmManager 时被当成“重启前未完成任务”恢复，
        // 向下一条本不该有任务的用例泄露一条未登记的 taskFeedback。见 SwarmTaskEditTest。
        if (_manager->taskController() && _manager->taskController()->running())
            _manager->taskController()->abort(QStringLiteral("测试清理"));
        _manager->shutdown();
    }
    _manager.reset();
    VehicleTest::cleanup();
}

void SwarmDispatchLockTest::lockedBuildMustReportNoAutodispatch()
{
    QVERIFY(!_manager->aiAutodispatchAvailable());
    QVERIFY(!_manager->deliveryNotice().isEmpty());
}

void SwarmDispatchLockTest::autonomyLevelMustBePinnedToSupervised()
{
    QCOMPARE(_manager->autonomyLevel(), 2);
    _manager->setAutonomyLevel(3);
    QCOMPARE(_manager->autonomyLevel(), 2);
    _manager->setAutonomyLevel(1);
    QCOMPARE(_manager->autonomyLevel(), 2);
    QVERIFY(_manager->l3AllowedTypes().isEmpty());
}

void SwarmDispatchLockTest::persistedAutonomyLevelMustBeIgnored()
{
    QSettings().setValue(QStringLiteral("Swarm/autonomyLevel"), 3);
    auto fresh = std::make_unique<SwarmManager>();
    QCOMPARE(fresh->autonomyLevel(), 2);
    QVERIFY(fresh->l3AllowedTypes().isEmpty());
    fresh->shutdown();
    QSettings().remove(QStringLiteral("Swarm/autonomyLevel"));
}

void SwarmDispatchLockTest::requireConfirmMustBePinnedOn()
{
    QVERIFY(_manager->aiAgent()->requireConfirm());
    _manager->aiAgent()->setRequireConfirm(false);
    QVERIFY(_manager->aiAgent()->requireConfirm());
}

void SwarmDispatchLockTest::harmlessAiPlanMustNotAutoDispatch()
{
    // pause 不是危险动作（见 SwarmAction::isDangerousType）：解锁构建在 L2 下会直接下发，
    // 锁定构建把 _requireConfirm 钉死为 true 后必须挂起待确认，一条都不能下发。
    ignoreLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("AI 已生成行动计划（含危险动作 [0-9]+ 项），请在面板确认执行")));
    const QString json = QStringLiteral(R"([{"type": "pause"}])");
    _manager->aiAgent()->handleActionsJsonForTest(QJsonDocument::fromJson(json.toUtf8()).array());

    QVERIFY(_manager->aiAgent()->planPending());
    QCOMPARE(mockLink()->receivedMavCommandCount(MAV_CMD_DO_REPOSITION), 0);
}

void SwarmDispatchLockTest::confirmedAiPlanMustDispatch()
{
    // 与上一条互为对照：锁定构建挂起的计划，操作员确认后必须能正常下发，否则“锁定”
    // 只是把确认通道一起锁死了，交付出去的构建将毫无用处。
    ignoreLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("AI 已生成行动计划（含危险动作 [0-9]+ 项），请在面板确认执行")));
    const QString json = QStringLiteral(R"([{"type": "pause"}])");
    _manager->aiAgent()->handleActionsJsonForTest(QJsonDocument::fromJson(json.toUtf8()).array());
    QVERIFY(_manager->aiAgent()->planPending());

    ignoreLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("操作员已确认，计划开始执行。")));
    // MockLink 不实现 MAV_CMD_DO_REPOSITION，会原样回 MAV_RESULT_UNSUPPORTED；命令仍然
    // 完整地经链路发出并计数（见 MockLink::_updateIncomingMessageCounts），这条反馈只是
    // 收到 ack 之后的副作用，与本用例要证明的“确认后必须下发”无关，按最窄范围忽略。
    ignoreLogMessage(
        "API.QGCApplication.AppMessage", QtDebugMsg,
        QRegularExpression(QStringLiteral("Reposition \\(MAV_CMD_DO_REPOSITION\\) command not supported")));
    _manager->aiAgent()->confirmPlan();
    QTRY_VERIFY_WITH_TIMEOUT(mockLink()->receivedMavCommandCount(MAV_CMD_DO_REPOSITION) > 0, TestTimeout::longMs());
}

void SwarmDispatchLockTest::operatorTaskMustStillRun()
{
    // 锁定构建只钉死 AI 自动下发；操作员亲自发起的多阶段任务不受影响，必须照常运行。
    // 同一条 MAV_CMD_DO_REPOSITION“不支持”副作用见 confirmedAiPlanMustDispatch 注释；
    // “测试清理”是 cleanup() 在 complete_when=manual 的任务仍在运行时调用 abort() 的
    // 中止原因文案（本用例故意不调用 proceed()，任务不会自行结束）。
    ignoreLogMessage(
        "API.QGCApplication.AppMessage", QtDebugMsg,
        QRegularExpression(QStringLiteral("Reposition \\(MAV_CMD_DO_REPOSITION\\) command not supported")));
    ignoreLogMessage("API.QGCApplication.AppMessage", QtDebugMsg, QRegularExpression(QStringLiteral("测试清理")));
    const QString json = QStringLiteral(R"([{"name": "人工任务",
        "actions": [{"type": "pause"}],
        "complete_when": {"kind": "manual", "timeout_sec": 600}}])");
    QStringList warnings;
    const auto stages = _manager->taskController()->parseStages(QJsonDocument::fromJson(json.toUtf8()).array(),
                                                                vehicle()->coordinate(), warnings);
    QVERIFY(warnings.isEmpty());
    QCOMPARE(stages.count(), 1);

    QVERIFY(_manager->taskController()->startTask(QStringLiteral("人工任务"), stages, true));
    QVERIFY(_manager->taskController()->running());
    QTRY_VERIFY_WITH_TIMEOUT(mockLink()->receivedMavCommandCount(MAV_CMD_DO_REPOSITION) > 0, TestTimeout::longMs());
}

void SwarmDispatchLockTest::modelAbortMustRequireConfirmation()
{
    // abort_swarm_task 曾从工具分发直达 taskController()->abort()，绕开自主等级闸和交付
    // 构建钉死的 _requireConfirm，使交付告示「所有 AI 动作经人工确认后执行」不成立。
    ignoreLogMessage(
        "API.QGCApplication.AppMessage", QtDebugMsg,
        QRegularExpression(QStringLiteral("Reposition \\(MAV_CMD_DO_REPOSITION\\) command not supported")));
    // 挂起走两条出口：_appendChat 进聊天区，agentFeedback 经 showAppMessage 弹提示；
    // 「已放弃 AI 行动计划」是 shutdown() 连带清掉这条挂起中止时发出的。
    ignoreLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral(
                         "(测试清理|模型请求中止当前任务|模型请求中止任务，请在面板确认|已放弃 AI 行动计划)")));
    _startOperatorTask();

    _manager->aiAgent()->handleActionsJsonForTest(
        QJsonDocument::fromJson(QByteArrayLiteral(R"([{"type": "abort_swarm_task"}])")).array());

    QVERIFY2(_manager->taskController()->running(), "模型请求的中止未经确认即生效");
    // 仅断言任务仍在运行不具区分度：请求被静默丢弃也会让任务继续跑。必须证明它确实挂起了。
    QVERIFY(_manager->aiAgent()->pendingAbort());
    QVERIFY(_manager->aiAgent()->planPending());
}

void SwarmDispatchLockTest::confirmedModelAbortMustStopTheTask()
{
    // 与上一条互为对照：挂起不能变成“中止永远不生效”，否则模型就丧失了提出中止的能力。
    ignoreLogMessage(
        "API.QGCApplication.AppMessage", QtDebugMsg,
        QRegularExpression(QStringLiteral("Reposition \\(MAV_CMD_DO_REPOSITION\\) command not supported")));
    ignoreLogMessage(
        "API.QGCApplication.AppMessage", QtDebugMsg,
        QRegularExpression(QStringLiteral("(模型请求中止当前任务|模型请求中止任务，请在面板确认|操作员已确认，"
                                          "任务中止|AI 请求、操作员确认中止)")));
    _startOperatorTask();
    _manager->aiAgent()->handleActionsJsonForTest(
        QJsonDocument::fromJson(QByteArrayLiteral(R"([{"type": "abort_swarm_task"}])")).array());
    QVERIFY(_manager->aiAgent()->pendingAbort());

    _manager->aiAgent()->confirmPlan();

    QVERIFY(!_manager->taskController()->running());
    QVERIFY(!_manager->aiAgent()->pendingAbort());
    QCOMPARE(_manager->taskController()->outcome(), SwarmTaskController::Outcome::Aborted);
}

void SwarmDispatchLockTest::staleAbortMustNotKillTheNextTask()
{
    // 任务成功结束时 SwarmTaskController::_finish 不调 cancelTargets，挂起态不会被清理。
    // 若中止请求不绑定目标任务，这条残留请求会在下一个任务上生效——一个已完成任务的
    // 模型中止请求可以杀掉后续无关任务，而面板上看不出它是过期的。
    ignoreLogMessage(
        "API.QGCApplication.AppMessage", QtDebugMsg,
        QRegularExpression(QStringLiteral("Reposition \\(MAV_CMD_DO_REPOSITION\\) command not supported")));
    ignoreLogMessage(
        "API.QGCApplication.AppMessage", QtDebugMsg,
        QRegularExpression(QStringLiteral("(测试清理|模型请求中止任务|中止请求已过期|已放弃 AI 行动计划)")));
    _startOperatorTask();
    _manager->aiAgent()->handleActionsJsonForTest(
        QJsonDocument::fromJson(QByteArrayLiteral(R"([{"type": "abort_swarm_task"}])")).array());
    QVERIFY(_manager->aiAgent()->pendingAbort());

    // 第一个任务必须**成功**结束才能复现：_finish 只在失败路径调 cancelTargets
    // （→ invalidatePendingWork → rejectPlan），成功路径不清理挂起态。用 abort 结束
    // 会走失败路径顺带清掉 _pendingAbort，测试就不再具备区分度——实测过，守卫移除后
    // 那种写法照样通过。complete_when=manual 的阶段由 proceed() 推进即告成功。
    // proceed() 在阶段尚未进入等待态时是空操作：必须先等动作派发完、条件开始监控。
    QTRY_VERIFY_WITH_TIMEOUT(_manager->taskController()->waitingForProceed(), TestTimeout::longMs());
    _manager->taskController()->proceed();
    QTRY_VERIFY_WITH_TIMEOUT(!_manager->taskController()->running(), TestTimeout::longMs());
    QCOMPARE(_manager->taskController()->outcome(), SwarmTaskController::Outcome::Succeeded);
    QVERIFY2(_manager->aiAgent()->pendingAbort(), "前提不成立：成功结束本不应清理挂起的中止请求");
    _startOperatorTask();
    const QString freshTaskId = _manager->taskController()->taskId();

    _manager->aiAgent()->confirmPlan();

    QVERIFY2(_manager->taskController()->running(), "过期的中止请求中止了另一个任务");
    QCOMPARE(_manager->taskController()->taskId(), freshTaskId);
    QVERIFY(!_manager->aiAgent()->pendingAbort());
}

void SwarmDispatchLockTest::_startOperatorTask()
{
    const QString json = QStringLiteral(R"([{"name": "人工任务",
        "actions": [{"type": "pause"}],
        "complete_when": {"kind": "manual", "timeout_sec": 600}}])");
    QStringList warnings;
    const auto stages = _manager->taskController()->parseStages(QJsonDocument::fromJson(json.toUtf8()).array(),
                                                                vehicle()->coordinate(), warnings);
    QVERIFY(warnings.isEmpty());
    QVERIFY(_manager->taskController()->startTask(QStringLiteral("人工任务"), stages, true));
    QVERIFY(_manager->taskController()->running());
}

UT_REGISTER_TEST(SwarmDispatchLockTest, TestLabel::Integration, TestLabel::Vehicle)
