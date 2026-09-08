#include "SwarmRegressionTest.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopeGuard>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtNetwork/QUdpSocket>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>

#include "Fact.h"
#include "FirmwarePlugin.h"
#include "FormationController.h"
#include "MissionItem.h"
#include "MissionManager.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "RCToParamDialogController.h"
#include "SwarmClock.h"
#include "SwarmManager.h"
#include "SwarmMissionPlanner.h"
#include "SwarmPerceptionBridge.h"
#include "SwarmTaskController.h"
#include "SwarmTaskLibrary.h"
#include "SwarmTunnelProtocol.h"
#include "Vehicle.h"
#include "VehicleFactGroup.h"

SwarmRegressionTest::~SwarmRegressionTest() = default;

void SwarmRegressionTest::init()
{
    setAutopilotType(QString::fromLatin1(QTest::currentDataTag()).contains("ArduPilot") ? MAV_AUTOPILOT_ARDUPILOTMEGA
                                                                                        : MAV_AUTOPILOT_PX4);
    VehicleTest::init();
    ignoreLogMessage("ComponentInformation.RequestMetaDataTypeStateMachine", QtWarningMsg,
                     QRegularExpression("failed to load metadata"));
    ignoreLogMessage("FirmwarePlugin.ParameterMetaData", QtWarningMsg,
                     QRegularExpression("Skipping invalid enum value"));
    QVERIFY(vehicle());
    ignoreLogMessage(
        "API.QGCApplication.AppMessage", QtDebugMsg,
        QRegularExpression("(集群任务|阶段 |阶段「|检测到重启前未完成任务|[0-9]+ 号机：|Reposition "
                           "\\(MAV_CMD_DO_REPOSITION\\) command not supported|operator stop probe|Mission transfer "
                           "failed. Error: Mission write mission count failed, maximum retries exceeded.)"));
    _configPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/SwarmConfig/fences.json";
    QFile file(_configPath);
    _hadConfig = file.exists();
    _previousConfig = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    file.close();
    QDir().mkpath(QFileInfo(_configPath).absolutePath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("{}"), 2);
    file.close();
    QFile::remove(QFileInfo(_configPath).absolutePath() + "/tasks.json");
    _manager = std::make_unique<SwarmManager>();
    _manager->setAutonomyLevel(2);
    mockLink()->clearReceivedMavCommandCounts();
}

void SwarmRegressionTest::cleanup()
{
    SwarmClock::setTestClock({});
    if (_manager)
        _manager->shutdown();
    _manager.reset();
    QFile::remove(QFileInfo(_configPath).absolutePath() + "/tasks.json");
    for (const auto& link : std::as_const(_extraLinks))
        if (link)
            link->disconnect();
    _extraLinks.clear();
    if (_secondaryLink) {
        MultiVehicleManager::instance()->setActiveVehicle(vehicle());
        _secondaryLink->disconnect();
        _secondaryLink = nullptr;
        QTRY_COMPARE_WITH_TIMEOUT(MultiVehicleManager::instance()->vehicles()->count(), 1, TestTimeout::longMs());
    }
    QFile file(_configPath);
    if (_hadConfig) {
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(_previousConfig), _previousConfig.size());
    } else
        file.remove();
    MultiVehicleManager::instance()->deselectAllVehicles();
    QTRY_COMPARE_WITH_TIMEOUT(MultiVehicleManager::instance()->vehicles()->count(), 1, TestTimeout::longMs());
    VehicleTest::cleanup();
}

void SwarmRegressionTest::addVehicles(int count)
{
    auto* mvm = MultiVehicleManager::instance();
    const int initial = mvm->vehicles()->count();
    for (int i = 0; i < count; ++i) {
        expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                         QRegularExpression("Connected to Vehicle [0-9]+"));
        _extraLinks.append(MockLink::startPX4MockLink());
        QTRY_COMPARE_WITH_TIMEOUT(mvm->vehicles()->count(), initial + i + 1, TestTimeout::longMs());
        for (int v = 0; v < mvm->vehicles()->count(); ++v) {
            auto* member = qobject_cast<Vehicle*>(mvm->vehicles()->get(v));
            QVERIFY(member);
            QTRY_VERIFY_WITH_TIMEOUT(member->isInitialConnectComplete(), TestTimeout::longMs());
        }
        verifyExpectedLogMessage();
    }
}

void SwarmRegressionTest::remainingMembersContinueLaterStages()
{
    addVehicles(1);
    QStringList errors;
    const auto arm =
        _manager->parseAndValidate({QJsonObject{{"type", "arm"}, {"arm", true}}}, vehicle()->coordinate(), errors);
    const auto disarm = _manager->parseAndValidate({QJsonObject{{"type", "disarm"}}}, vehicle()->coordinate(), errors);
    QVERIFY2(!arm.isEmpty() && !disarm.isEmpty(), qPrintable(errors.join(";")));
    auto* task = _manager->taskController();
    SwarmTaskController::Stage first{"arm members", arm, "manual", {}};
    SwarmTaskController::Stage second{"disarm remaining", disarm, "all_disarmed", {{"grace_sec", 0}}};
    QVERIFY(task->startTask("partial withdrawal", {first, second}, true));
    QTRY_VERIFY_WITH_TIMEOUT(task->waitingForProceed(), TestTimeout::longMs());
    QVERIFY(vehicle()->armed());
    auto* mvm = MultiVehicleManager::instance();
    mvm->deselectAllVehicles();
    mvm->selectVehicle(vehicle()->id());
    _manager->pauseAll();
    QVERIFY(task->running());
    mockLink()->clearReceivedMavCommandCounts();
    QSignalSpy finished(task, &SwarmTaskController::taskFinished);
    QSignalSpy executions(_manager.get(), &SwarmManager::executionFinished);
    task->proceed();
    QTRY_VERIFY_WITH_TIMEOUT(!executions.isEmpty(), 15000);
    QVERIFY2(executions.last()[1].toBool(), qPrintable(executions.last()[2].toString()));
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 5000);
    QVERIFY(finished.first()[0].toBool());
    QVERIFY(vehicle()->armed());
    QCOMPARE(mockLink()->receivedMavCommandCount(MAV_CMD_COMPONENT_ARM_DISARM), 0);
    QVERIFY(task->stateText().contains(QStringLiteral("退出")));
}

void SwarmRegressionTest::withdrawnGroupDoesNotBlockFutureGroups()
{
    addVehicles(1);
    auto* mvm = MultiVehicleManager::instance();
    QPointer<Vehicle> remaining;
    for (int i = 0; i < mvm->vehicles()->count(); ++i) {
        auto* target = qobject_cast<Vehicle*>(mvm->vehicles()->get(i));
        if (target && target != vehicle())
            remaining = target;
    }
    QVERIFY(remaining);
    SwarmAction first;
    first.type = "arm";
    first.params = {{"arm", true}};
    first.targetIds = {vehicle()->id()};
    SwarmAction second = first;
    second.targetIds = {remaining->id()};
    SwarmAction final = second;
    final.type = "disarm";
    auto* task = _manager->taskController();
    QVERIFY(task->startTask("group withdrawal",
                            {{"first group", {first}, "manual", {}},
                             {"future group", {second}, "manual", {}},
                             {"finish remaining", {final}, "all_disarmed", {{"grace_sec", 0}}}},
                            true));
    QTRY_VERIFY_WITH_TIMEOUT(task->waitingForProceed(), TestTimeout::longMs());
    mvm->deselectAllVehicles();
    mvm->selectVehicle(vehicle()->id());
    _manager->pauseAll();
    mockLink()->clearReceivedMavCommandCounts();
    QTRY_COMPARE_WITH_TIMEOUT(task->currentStageIndex(), 1, TestTimeout::longMs());
    QTRY_VERIFY_WITH_TIMEOUT(task->waitingForProceed(), TestTimeout::longMs());
    QVERIFY(remaining && remaining->armed());
    task->proceed();
    QTRY_VERIFY_WITH_TIMEOUT(!task->running(), TestTimeout::longMs());
    QVERIFY(remaining && !remaining->armed());
    QVERIFY(vehicle()->armed());
    QCOMPARE(mockLink()->receivedMavCommandCount(MAV_CMD_COMPONENT_ARM_DISARM), 0);
}

void SwarmRegressionTest::legacyTemplateMigrationPreservesCustomEdits()
{
    QFile fixture(QFINDTESTDATA("fixtures/legacy_relay_template.json"));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    const auto legacy = fixture.readAll();
    const QString path = _manager->taskLibrary()->templateDir() + QStringLiteral("/多机接力值守.json");
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto original = file.readAll();
    file.close();
    const auto restore = qScopeGuard([path, original] {
        QFile saved(path);
        if (saved.open(QIODevice::WriteOnly))
            saved.write(original);
    });
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(legacy), legacy.size());
    file.close();
    {
        SwarmTaskLibrary migrated(_manager.get());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll()).object().value("builtin_revision").toInt(), 2);
        file.close();
        QFile backup(path + ".pre-revision-2.bak");
        QVERIFY(backup.open(QIODevice::ReadOnly));
        QCOMPARE(backup.readAll(), legacy);
    }
    auto custom = QJsonDocument::fromJson(legacy).object();
    custom["description"] = "operator custom settings";
    const auto customBytes = QJsonDocument(custom).toJson();
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(customBytes), customBytes.size());
    file.close();
    SwarmTaskLibrary preserved(_manager.get());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), customBytes);
}

void SwarmRegressionTest::allTemplatesUseRuntimeContracts()
{
    addVehicles(3);
    QString error;
    const QStringList names{
        QStringLiteral("区域搜索后返航"), QStringLiteral("编队巡航后返航"), QStringLiteral("快速出动待命"),
        QStringLiteral("全员返航降落"),   QStringLiteral("扇形扫掠搜索"),   QStringLiteral("平行网格扫掠"),
        QStringLiteral("编队巡逻"),       QStringLiteral("多机接力值守"),   QStringLiteral("会合集结")};
    for (const auto& name : names) {
        const auto stages = _manager->taskLibrary()->instantiate(name, {}, error);
        QVERIFY2(!stages.isEmpty() && error.isEmpty(), qPrintable(name + ": " + error));
        QStringList errors;
        QVERIFY2(!_manager->taskController()->parseStages(stages, vehicle()->coordinate(), errors).isEmpty(),
                 qPrintable(errors.join(";")));
    }
    const auto longStages =
        _manager->taskLibrary()->instantiate(QStringLiteral("编队巡航后返航"), {{"cruise_seconds", "7200"}}, error);
    QVERIFY2(!longStages.isEmpty(), qPrintable(error));
    const auto duration = longStages[2].toObject().value("complete_when").toObject();
    QVERIFY(duration.value("timeout_sec").toInt() > 7200);
    const auto quick = _manager->taskLibrary()->instantiate(QStringLiteral("快速出动待命"), {{"altitude", "5"}}, error);
    QVERIFY2(!quick.isEmpty(), qPrintable(error));
    QVERIFY(quick[0].toObject().value("complete_when").toObject().value("min_altitude").toDouble() <= 5);
    for (const auto& bad :
         {QStringLiteral("12abc"), QStringLiteral("NaN"), QStringLiteral(""), QStringLiteral("121")}) {
        QVERIFY(
            _manager->taskLibrary()->instantiate(QStringLiteral("快速出动待命"), {{"altitude", bad}}, error).isEmpty());
        QVERIFY(!error.isEmpty());
    }
    const QVariantList overlap{vehicle()->id()};
    QVERIFY(
        _manager->taskLibrary()
            ->instantiate(QStringLiteral("多机接力值守"), {{"group_a_ids", overlap}, {"group_b_ids", overlap}}, error)
            .isEmpty());
    QVERIFY(!error.isEmpty());
}

void SwarmRegressionTest::queuedArmingMustBeCancelledByPause_data()
{
    QTest::addColumn<bool>("actionPipeline");
    QTest::newRow("direct") << false;
    QTest::newRow("confirmed action") << true;
}

void SwarmRegressionTest::queuedArmingMustBeCancelledByPause()
{
    QFETCH(bool, actionPipeline);
    auto* mvm = MultiVehicleManager::instance();
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg, QRegularExpression("Connected to Vehicle [0-9]+"));
    _secondaryLink = MockLink::startPX4MockLink();
    QTRY_COMPARE_WITH_TIMEOUT(mvm->vehicles()->count(), 2, TestTimeout::longMs());
    Vehicle* secondary = nullptr;
    for (int i = 0; i < mvm->vehicles()->count(); ++i) {
        auto* candidate = qobject_cast<Vehicle*>(mvm->vehicles()->get(i));
        if (candidate && candidate != vehicle())
            secondary = candidate;
    }
    QVERIFY(secondary);
    QTRY_VERIFY_WITH_TIMEOUT(secondary->isInitialConnectComplete(), TestTimeout::longMs());
    verifyExpectedLogMessage();
    _secondaryLink->clearReceivedMavCommandCounts();
    _manager->setCommandIntervalMs(1500);
    _manager->armAll(true);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->armed(), 1200);
    QVERIFY(!secondary->armed());
    if (actionPipeline) {
        QStringList errors;
        const auto pause =
            _manager->parseAndValidate({QJsonObject{{"type", "pause"}}}, vehicle()->coordinate(), errors);
        QVERIFY(!pause.isEmpty());
        QVERIFY(_manager->executeActions(pause, "ai_confirmed", SwarmManager::ExecutionApproval::OperatorConfirmed));
    } else {
        _manager->pauseAll();
    }
    QTimer observation;
    observation.setSingleShot(true);
    QSignalSpy elapsed(&observation, &QTimer::timeout);
    observation.start(2200);
    QVERIFY(elapsed.wait(3500));
    const int lateArms = _secondaryLink->receivedMavCommandCount(MAV_CMD_COMPONENT_ARM_DISARM);
    QVERIFY2(
        !secondary->armed() && lateArms == 0,
        qPrintable(QString("After pause: secondary armed=%1, arm commands=%2").arg(secondary->armed()).arg(lateArms)));
}

void SwarmRegressionTest::taskAbortMustStopFormation()
{
    _manager->armAll(true);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->armed(), TestTimeout::longMs());
    QStringList errors;
    const auto actions = _manager->parseAndValidate(
        {QJsonObject{
            {"type", "start_formation"}, {"formation", "wedge"}, {"leader_id", vehicle()->id()}, {"spacing", 20}}},
        vehicle()->coordinate(), errors);
    QVERIFY2(!actions.isEmpty(), qPrintable(errors.join("; ")));
    SwarmTaskController::Stage stage;
    stage.name = "formation hold";
    stage.actions = actions;
    stage.conditionKind = "manual";
    auto* task = _manager->taskController();
    QVERIFY(task->startTask("abort probe", {stage}, true));
    QTRY_VERIFY_WITH_TIMEOUT(_manager->formation()->active(), TestTimeout::longMs());
    task->abort("operator stop probe");
    QVERIFY(!task->running());
    QVERIFY2(!_manager->formation()->active(), "Task is stopped but formation timer remains active");
}

void SwarmRegressionTest::neverStartedMissionMustNotComplete()
{
    QVERIFY(!vehicle()->armed());
    QStringList errors;
    SwarmTaskController::Stage stage;
    stage.name = "await mission completion";
    stage.actions = _manager->parseAndValidate({QJsonObject{{"type", "pause"}}}, vehicle()->coordinate(), errors);
    QVERIFY(!stage.actions.isEmpty());
    stage.conditionKind = "all_mission_finished";
    stage.conditionParams = {{"grace_sec", 0}, {"timeout_sec", 10}};
    auto* task = _manager->taskController();
    QSignalSpy finished(task, &SwarmTaskController::taskFinished);
    QVERIFY(task->startTask("never started probe", {stage}, true));
    QTimer observation;
    observation.setSingleShot(true);
    QSignalSpy elapsed(&observation, &QTimer::timeout);
    observation.start(1500);
    QVERIFY(elapsed.wait(3000));
    QVERIFY2(task->running() && finished.isEmpty(), "A vehicle that never entered Mission was reported complete");
}

void SwarmRegressionTest::invalidTemplateNumberMustBeRejected()
{
    QString error;
    const auto stages = _manager->taskLibrary()->instantiate("快速出动待命", {{"altitude", "abc"}}, error);
    QVERIFY(stages.isEmpty());
    QVERIFY(!error.isEmpty());
}

void SwarmRegressionTest::coverageRoundTripMustRetainTakeoff()
{
    QStringList errors;
    const auto plan = _manager->parseAndValidate(
        {QJsonObject{{"type", "plan_coverage"}, {"length", 30}, {"width", 30}}}, vehicle()->coordinate(), errors);
    QVERIFY2(!plan.isEmpty(), qPrintable(errors.join("; ")));
    QSignalSpy uploaded(_manager->missionPlanner(), &SwarmMissionPlanner::missionUploaded);
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression("区域覆盖任务已提交上传请求：1 架无人机"));
    QVERIFY(_manager->executeActions(plan, "test", SwarmManager::ExecutionApproval::OperatorConfirmed));
    QTRY_COMPARE_WITH_TIMEOUT(uploaded.count(), 1, TestTimeout::longMs());
    verifyExpectedLogMessage();
    QTRY_VERIFY_WITH_TIMEOUT(!vehicle()->missionManager()->inProgress(), TestTimeout::longMs());
    const auto& items = vehicle()->missionManager()->missionItems();
    QVERIFY(!items.isEmpty());
    bool takeoffFound = false;
    QStringList commands;
    for (const auto* item : items) {
        takeoffFound |= item->command() == MAV_CMD_NAV_TAKEOFF;
        commands.append(QString::number(item->command()));
    }
    QVERIFY2(takeoffFound, qPrintable("PX4 round-trip commands: " + commands.join(", ")));
}

void SwarmRegressionTest::coverageRoundTripMustRetainTakeoff_data()
{
    TestData::addAutopilotRows();
}

void SwarmRegressionTest::missionCompletionRequiresCurrentExecutionEvidence()
{
    coverageRoundTripMustRetainTakeoff();
    QStringList errors;
    const auto actions =
        _manager->parseAndValidate({QJsonObject{{"type", "start_mission"}}}, vehicle()->coordinate(), errors);
    QVERIFY(!actions.isEmpty());
    auto* task = _manager->taskController();
    SwarmTaskController::Stage stage{"mission", actions, "all_mission_finished", {{"grace_sec", 0}}};
    QSignalSpy completed(task, &SwarmTaskController::taskFinished);
    QSignalSpy executed(_manager.get(), &SwarmManager::executionFinished);
    QVERIFY(task->startTask("mission evidence", {stage}, true));
    QTRY_VERIFY_WITH_TIMEOUT(!executed.isEmpty(), 15000);
    QVERIFY2(executed.last()[1].toBool(), qPrintable(executed.last()[2].toString()));
    auto publish = [this](int state) {
        mavlink_mission_current_t current{};
        current.mission_state = state;
        current.seq = 1;
        mavlink_message_t message{};
        mavlink_msg_mission_current_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &current);
        vehicle()->mavlinkMessageReceived(message);
    };
    publish(MISSION_STATE_COMPLETE);
    QVERIFY(QMetaObject::invokeMethod(task, "_tick"));
    QVERIFY(completed.isEmpty());
    publish(MISSION_STATE_ACTIVE);
    publish(MISSION_STATE_COMPLETE);
    QVERIFY(QMetaObject::invokeMethod(task, "_tick"));
    QCOMPARE(completed.count(), 1);
    QVERIFY(completed.first()[0].toBool());
    QVERIFY(!task->confirmMissionCompletion(task->executionToken()));
    executed.clear();
    QVERIFY(task->startTask("manual mission evidence", {stage}, true));
    QTRY_VERIFY_WITH_TIMEOUT(!executed.isEmpty(), 15000);
    QVERIFY(executed.last()[1].toBool());
    QVERIFY(QMetaObject::invokeMethod(task, "_tick"));
    QVERIFY(task->awaitingMissionConfirmation());
    const auto token = task->executionToken();
    QVERIFY(!task->confirmMissionCompletion(QStringLiteral("stale-token")));
    publish(MISSION_STATE_PAUSED);
    QVERIFY(!task->confirmMissionCompletion(token));
    publish(MISSION_STATE_UNKNOWN);
    QVERIFY(task->confirmMissionCompletion(token));
    QCOMPARE(completed.count(), 2);
}

void SwarmRegressionTest::durationUsesMonotonicDeadline()
{
    auto now = std::make_shared<qint64>(10000);
    SwarmClock::setTestClock([now] { return *now; });
    auto* task = _manager->taskController();
    QSignalSpy execution(_manager.get(), &SwarmManager::executionFinished);
    QSignalSpy finished(task, &SwarmTaskController::taskFinished);
    QVERIFY(task->startTask("monotonic duration",
                            {{"hold", {}, "duration", {{"seconds", 100}, {"grace_sec", 0}, {"timeout_sec", 200}}}},
                            true));
    QTRY_VERIFY_WITH_TIMEOUT(!execution.isEmpty(), TestTimeout::longMs());
    *now += 99000;
    QVERIFY(QMetaObject::invokeMethod(task, "_tick"));
    QVERIFY(finished.isEmpty());
    *now += 1000;
    QVERIFY(QMetaObject::invokeMethod(task, "_tick"));
    QCOMPARE(finished.count(), 1);
    QVERIFY(finished.first()[0].toBool());
}

void SwarmRegressionTest::lateTakeoffAcknowledgementCannotArmWithdrawnTarget()
{
    QObject context;
    bool allowed = true;
    int results = 0;
    bool success = true;
    vehicle()->firmwarePlugin()->takeoffAsync(
        vehicle(), 10, &context, [&allowed] { return allowed; },
        [&results, &success](bool ok) {
            ++results;
            success = ok;
        });
    allowed = false;
    vehicle()->mavCommandResult(vehicle()->id(), vehicle()->defaultComponentId(), MAV_CMD_NAV_TAKEOFF,
                                MAV_RESULT_ACCEPTED, 0);
    QCOMPARE(results, 1);
    QVERIFY(!success);
    vehicle()->mavCommandResult(vehicle()->id(), vehicle()->defaultComponentId(), MAV_CMD_NAV_TAKEOFF,
                                MAV_RESULT_ACCEPTED, 0);
    QCOMPARE(results, 1);
    QTimer observation;
    observation.setSingleShot(true);
    QSignalSpy elapsed(&observation, &QTimer::timeout);
    observation.start(150);
    QVERIFY(elapsed.wait(2000));
    QCOMPARE(mockLink()->receivedMavCommandCount(MAV_CMD_COMPONENT_ARM_DISARM), 0);
    QVERIFY(!vehicle()->armed());
}

void SwarmRegressionTest::missionReplacementBlocksLaterStart()
{
    QStringList errors;
    const auto coverage = _manager->parseAndValidate(
        {QJsonObject{{"type", "plan_coverage"}, {"length", 30}, {"width", 30}}}, vehicle()->coordinate(), errors);
    const auto start =
        _manager->parseAndValidate({QJsonObject{{"type", "start_mission"}}}, vehicle()->coordinate(), errors);
    QVERIFY(!coverage.isEmpty() && !start.isEmpty());
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression("区域覆盖任务已提交上传请求：1 架无人机"));
    auto* task = _manager->taskController();
    QVERIFY(task->startTask("revision guard",
                            {{"upload", coverage, "manual", {}}, {"start", start, "all_mission_finished", {}}}, true));
    QTRY_VERIFY_WITH_TIMEOUT(task->waitingForProceed(), TestTimeout::longMs());
    verifyExpectedLogMessage();
    auto* mission = vehicle()->missionManager();
    QSignalSpy read(mission, &MissionManager::newMissionItemsAvailable);
    mission->loadFromVehicle();
    QTRY_VERIFY_WITH_TIMEOUT(!read.isEmpty(), TestTimeout::longMs());
    mockLink()->clearReceivedMavCommandCounts();
    task->proceed();
    QVERIFY(task->blocked());
    QVERIFY(!vehicle()->armed());
    QCOMPARE(mockLink()->receivedMavCommandCount(MAV_CMD_COMPONENT_ARM_DISARM), 0);
}

void SwarmRegressionTest::formationLeaderWithdrawalKeepsRemainingMembers()
{
    addVehicles(2);
    _manager->armAll(true);
    auto* mvm = MultiVehicleManager::instance();
    QList<int> members;
    for (int i = 0; i < mvm->vehicles()->count(); ++i) {
        auto* target = qobject_cast<Vehicle*>(mvm->vehicles()->get(i));
        QVERIFY(target);
        QTRY_VERIFY_WITH_TIMEOUT(target->armed(), TestTimeout::longMs());
        members.append(target->id());
    }
    // Supply airborne telemetry to exercise membership policy without a flight dynamics simulator.
    for (int id : members) {
        auto* target = mvm->getVehicleById(id);
        QVERIFY(target);
        target->altitudeRelative()->setRawValue(20);
    }
    const int previousLeader = vehicle()->id();
    QVERIFY(_manager->startFormation(previousLeader));
    _manager->cancelTargets({previousLeader}, "leader withdrawal");
    QVERIFY(_manager->formation()->active());
    QVERIFY(_manager->formation()->leaderVehicleId() != previousLeader);
    members.removeAll(previousLeader);
    QVERIFY(members.contains(_manager->formation()->leaderVehicleId()));
    members.removeAll(_manager->formation()->leaderVehicleId());
    _manager->cancelTargets(members, "last follower withdrawal");
    QVERIFY(!_manager->formation()->active());
}

void SwarmRegressionTest::missionMustWaitForUploadAcknowledgement()
{
    mockLink()->setMissionItemFailureMode(MockLinkMissionItemHandler::FailWriteMissionCountNoResponse,
                                          MAV_MISSION_ACCEPTED);
    QStringList errors;
    const auto plan = _manager->parseAndValidate(
        {QJsonObject{{"type", "plan_coverage"}, {"length", 30}, {"width", 30}}, QJsonObject{{"type", "start_mission"}}},
        vehicle()->coordinate(), errors);
    QVERIFY2(!plan.isEmpty(), qPrintable(errors.join("; ")));
    QSignalSpy uploaded(_manager->missionPlanner(), &SwarmMissionPlanner::missionUploaded);
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression("区域覆盖任务已提交上传请求：1 架无人机"));
    QVERIFY(_manager->executeActions(plan, "test", SwarmManager::ExecutionApproval::OperatorConfirmed));
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->missionManager()->inProgress(), TestTimeout::longMs());
    verifyExpectedLogMessage();
    QSignalSpy finished(_manager.get(), &SwarmManager::executionFinished);
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), TestTimeout::longMs());
    QVERIFY(!finished.first().at(1).toBool());
    QVERIFY(uploaded.isEmpty());
    QVERIFY(vehicle()->flightMode() != vehicle()->missionFlightMode());
    mockLink()->setMissionItemFailureMode(MockLinkMissionItemHandler::FailNone, MAV_MISSION_ACCEPTED);
}

UT_REGISTER_TEST(SwarmRegressionTest, TestLabel::Integration, TestLabel::Vehicle)
