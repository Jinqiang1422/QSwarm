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

UT_REGISTER_TEST(SwarmDispatchLockTest, TestLabel::Integration, TestLabel::Vehicle)
