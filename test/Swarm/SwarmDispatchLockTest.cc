#include "SwarmDispatchLockTest.h"

#include <QtCore/QRegularExpression>

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

UT_REGISTER_TEST(SwarmDispatchLockTest, TestLabel::Integration, TestLabel::Vehicle, TestLabel::SwarmLocked)
