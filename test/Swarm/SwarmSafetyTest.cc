#include "SwarmSafetyTest.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <limits>

#include "MissionItem.h"
#include "MissionManager.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "SwarmCOPModel.h"
#include "SwarmEventDetector.h"
#include "SwarmManager.h"
#include "SwarmMissionPlanner.h"
#include "SwarmMissionSnapshot.h"
#include "SwarmTaskController.h"
#include "SwarmTrackFusion.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"

SwarmSafetyTest::~SwarmSafetyTest() = default;

void SwarmSafetyTest::init()
{
    VehicleTest::init();
    // MockLink does not serve COMP_METADATA_TYPE_GENERAL; the primary/fallback download
    // failure is environmental (this host has restricted network access), not a defect
    // under test. Whether the attempt fails inside this test's strict-log capture window
    // is timing-dependent, which otherwise makes an unrelated safety test flake. See
    // QmlUITestBase::ignoreAPMMockLinkWarnings for the identical precedent.
    ignoreLogMessage("ComponentInformation.RequestMetaDataTypeStateMachine", QtWarningMsg,
                     QRegularExpression(QStringLiteral(
                         "\"COMP_METADATA_TYPE_GENERAL\" : failed to load metadata \\(primary and fallback\\) \"\"")));
    QVERIFY(vehicle());
    ignoreLogMessage(
        "API.QGCApplication.AppMessage", QtDebugMsg,
        QRegularExpression(
            "(集群任务|阶段 |阶段「|检测到重启前未完成任务|区域覆盖任务已提交上传请求|已请求清空|[0-9]+ 号机：)"));
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

void SwarmSafetyTest::cleanup()
{
    if (_manager)
        _manager->shutdown();
    _manager.reset();
    QFile::remove(QFileInfo(_configPath).absolutePath() + "/tasks.json");
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
    VehicleTest::cleanup();
}

void SwarmSafetyTest::writeFence(const QByteArray& json)
{
    QFile file(_configPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(json), json.size());
}

void SwarmSafetyTest::addSecondVehicle()
{
    auto* mvm = MultiVehicleManager::instance();
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg, QRegularExpression("Connected to Vehicle [0-9]+"));
    _secondaryLink = MockLink::startPX4MockLink();
    QTRY_COMPARE_WITH_TIMEOUT(mvm->vehicles()->count(), 2, TestTimeout::longMs());
    for (int i = 0; i < mvm->vehicles()->count(); ++i) {
        auto* target = qobject_cast<Vehicle*>(mvm->vehicles()->get(i));
        QVERIFY(target);
        QTRY_VERIFY_WITH_TIMEOUT(target->isInitialConnectComplete(), TestTimeout::longMs());
        // 初连完成不等于 SwarmManager 眼里的“在线”：_vehicleStates 取的是
        // !communicationLost()，而心跳在机器繁忙时会迟到。少等这一步，_planViolations 就会
        // 判出 error 级的“目标机缺失或离线”，executeActions 直接返回 false——表现为满载下
        // 偶发的功能性失败，且与被测的策略撤销逻辑毫无关系。
        QTRY_VERIFY_WITH_TIMEOUT(!target->vehicleLinkManager()->communicationLost(), TestTimeout::longMs());
    }
    verifyExpectedLogMessage();
    _secondaryLink->clearReceivedMavCommandCounts();
}

void SwarmSafetyTest::policyRevocationMustStopQueuedAutomaticArm_data()
{
    QTest::addColumn<bool>("downgrade");
    QTest::addColumn<bool>("confirmed");
    QTest::addColumn<bool>("manual");
    QTest::addColumn<int>("level");
    QTest::addColumn<bool>("continues");
    QTest::newRow("L3-to-L2") << true << false << false << 2 << false;
    QTest::newRow("remove-arm-from-L3-whitelist") << false << false << false << 3 << false;
    QTest::newRow("confirmed-L3-to-L2") << true << true << false << 2 << true;
    QTest::newRow("confirmed-remove-whitelist") << false << true << false << 3 << true;
    QTest::newRow("confirmed-L3-to-L1") << true << true << false << 1 << false;
    QTest::newRow("manual-L3-to-L1") << true << true << true << 1 << true;
}

void SwarmSafetyTest::policyRevocationMustStopQueuedAutomaticArm()
{
    QFETCH(bool, downgrade);
    QFETCH(bool, confirmed);
    QFETCH(bool, manual);
    QFETCH(int, level);
    QFETCH(bool, continues);
    addSecondVehicle();
    _manager->setAutonomyLevel(3);
    _manager->setL3AllowedTypes({"arm"});
    _manager->setCommandIntervalMs(1500);
    _manager->authorizeTaskTargets(_manager->boundTargetIds());
    QStringList errors;
    const auto actions =
        _manager->parseAndValidate({QJsonObject{{"type", "arm"}, {"arm", true}}}, vehicle()->coordinate(), errors);
    QVERIFY2(!actions.isEmpty(), qPrintable(errors.join(";")));
    QVERIFY(!_manager->needsConfirmation(actions));
    QSignalSpy finished(_manager.get(), &SwarmManager::executionFinished);
    QVERIFY(_manager->executeActions(
        actions, manual ? "operator" : "ai",
        confirmed ? SwarmManager::ExecutionApproval::OperatorConfirmed : SwarmManager::ExecutionApproval::Automatic));
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->armed(), 1200);
    QCOMPARE(_secondaryLink->receivedMavCommandCount(MAV_CMD_COMPONENT_ARM_DISARM), 0);
    if (downgrade)
        _manager->setAutonomyLevel(level);
    else
        _manager->setL3AllowedTypes({});
    QVERIFY(_manager->needsConfirmation(actions));
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 15000);
    const int lateArms = _secondaryLink->receivedMavCommandCount(MAV_CMD_COMPONENT_ARM_DISARM);
    QCOMPARE(lateArms > 0, continues);
    QCOMPARE(finished.last()[1].toBool(), continues);
}

void SwarmSafetyTest::queuedTakeoffMustRecheckChangedAltitudeLimit()
{
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("(Failed to connect to pressure backend|Error Initializing Pressure Sensor)"));
    addSecondVehicle();
    QStringList errors;
    const auto actions = _manager->parseAndValidate({QJsonObject{{"type", "takeoff"}, {"altitude", 20}}},
                                                    vehicle()->coordinate(), errors);
    QVERIFY2(!actions.isEmpty(), qPrintable(errors.join(";")));
    QSignalSpy finished(_manager.get(), &SwarmManager::executionFinished);
    QVERIFY(_manager->executeActions(actions, "test", SwarmManager::ExecutionApproval::OperatorConfirmed));
    QTRY_VERIFY_WITH_TIMEOUT(mockLink()->receivedMavCommandCount(MAV_CMD_NAV_TAKEOFF) > 0, 1200);
    QCOMPARE(_secondaryLink->receivedMavCommandCount(MAV_CMD_NAV_TAKEOFF), 0);
    writeFence(R"({"max_altitude_m":3})");
    QVERIFY(_manager->planHasConstraintError(actions));
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 15000);
    const int lateTakeoffs = _secondaryLink->receivedMavCommandCount(MAV_CMD_NAV_TAKEOFF);
    QCOMPARE(lateTakeoffs, 0);
}

void SwarmSafetyTest::existingMissionMustRecheckChangedAltitudeLimit()
{
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->homePosition().isValid(), TestTimeout::longMs());
    QStringList errors;
    const auto coverage = _manager->parseAndValidate(
        {QJsonObject{{"type", "plan_coverage"}, {"length", 30}, {"width", 30}, {"altitude", 20}}},
        vehicle()->coordinate(), errors);
    const auto start =
        _manager->parseAndValidate({QJsonObject{{"type", "start_mission"}}}, vehicle()->coordinate(), errors);
    QVERIFY2(!coverage.isEmpty() && !start.isEmpty(), qPrintable(errors.join(";")));
    auto* task = _manager->taskController();
    QVERIFY(task->startTask("upload then constraints change",
                            {{"upload", coverage, "manual", {}}, {"start", start, "manual", {}}}, true));
    QTRY_VERIFY_WITH_TIMEOUT(task->waitingForProceed(), 15000);
    QVERIFY(vehicle()->missionManager()->missionItems().count() > 0);
    writeFence(R"({"max_altitude_m":3})");
    QVERIFY(_manager->planHasConstraintError(coverage));
    mockLink()->clearReceivedMavCommandCounts();
    QSignalSpy finished(_manager.get(), &SwarmManager::executionFinished);
    task->proceed();
    QTRY_VERIFY_WITH_TIMEOUT(task->blocked() || !finished.isEmpty(), 15000);
    const int arms = mockLink()->receivedMavCommandCount(MAV_CMD_COMPONENT_ARM_DISARM);
    QCOMPARE(arms, 0);
}

void SwarmSafetyTest::clearingBusyMissionMustNotReportSuccess()
{
    auto* mission = vehicle()->missionManager();
    mockLink()->setMissionItemFailureMode(MockLinkMissionItemHandler::FailReadRequestListNoResponse,
                                          MAV_MISSION_ACCEPTED);
    mission->loadFromVehicle();
    QVERIFY(mission->inProgress());
    QStringList errors;
    const auto actions =
        _manager->parseAndValidate({QJsonObject{{"type", "clear_missions"}}}, vehicle()->coordinate(), errors);
    QVERIFY2(!actions.isEmpty(), qPrintable(errors.join(";")));
    QSignalSpy cleared(mission, &MissionManager::removeAllComplete);
    QSignalSpy finished(_manager.get(), &SwarmManager::executionFinished);
    QVERIFY(_manager->executeActions(actions, "test", SwarmManager::ExecutionApproval::OperatorConfirmed));
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 2000);
    mockLink()->setMissionItemFailureMode(MockLinkMissionItemHandler::FailNone, MAV_MISSION_ACCEPTED);
    QVERIFY2(!finished.last()[1].toBool(),
             "PlanManager ignored removeAll while busy, but Swarm execution reported success");
}

void SwarmSafetyTest::terminalNotificationMustPreserveNewTask_data()
{
    QTest::addColumn<int>("notification");
    QTest::newRow("taskChanged") << 0;
    QTest::newRow("taskResultReady") << 1;
    QTest::newRow("taskFinished") << 2;
}

void SwarmSafetyTest::clearMustVerifyEmptyReadback()
{
    QStringList errors;
    const auto coverage = _manager->parseAndValidate(
        {QJsonObject{{"type", "plan_coverage"}, {"length", 30}, {"width", 30}}}, vehicle()->coordinate(), errors);
    QSignalSpy finished(_manager.get(), &SwarmManager::executionFinished);
    QVERIFY(_manager->executeActions(coverage, "operator", SwarmManager::ExecutionApproval::OperatorConfirmed));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
    QVERIFY(finished.first()[1].toBool());
    auto* mission = vehicle()->missionManager();
    QVERIFY(!mission->missionItems().isEmpty());
    QSignalSpy cleared(mission, &MissionManager::removeAllComplete);
    QSignalSpy read(mission, &MissionManager::newMissionItemsAvailable);
    QVERIFY(_manager->clearAllMissions());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 15000);
    QVERIFY2(finished.last()[1].toBool(), qPrintable(finished.last()[2].toString()));
    QCOMPARE(cleared.size(), 1);
    QVERIFY(!cleared.first()[0].toBool());
    QVERIFY(std::any_of(read.cbegin(), read.cend(), [](const auto& signal) { return !signal[0].toBool(); }));
    QVERIFY(mission->missionItems().isEmpty());
    QVERIFY(!mission->inProgress());
    QCOMPARE(_manager->missionPlanner()->uploadStates().value(QString::number(vehicle()->id())).toString(), "verified");
}

void SwarmSafetyTest::uploadNotificationMustNotCompleteNewReadback()
{
    SwarmMissionPlanner planner;
    const int id = vehicle()->id();
    quint64 replacementBatch = 0;
    int replacementReads = 0;
    QObject context;
    QSignalSpy finished(&planner, &SwarmMissionPlanner::batchFinished);
    connect(&planner, &SwarmMissionPlanner::missionUploaded, &context, [&](int uploadedId, int) {
        QCOMPARE(uploadedId, id);
        planner.cancelTargets({id});
        connect(vehicle()->missionManager(), &MissionManager::newMissionItemsAvailable, &context,
                [&](bool removed) { replacementReads += !removed; });
        QVERIFY(planner.readMissionBatch({id}, false, {}));
        replacementBatch = planner.batchId();
    });
    QVERIFY(planner.planAndUploadCoverage(vehicle()->coordinate(), 30, 30, 0));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 15000);
    QVERIFY(replacementBatch > 0);
    QCOMPARE(finished.last()[0].toULongLong(), replacementBatch);
    QVERIFY(finished.last()[1].toBool());
    QCOMPARE(replacementReads, 1);
    QVERIFY(!planner.verifiedSnapshot(id).empty());
}

void SwarmSafetyTest::missionSnapshotValidation()
{
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->homePosition().isValid(), TestTimeout::longMs());
    const auto coordinate = vehicle()->coordinate();
    MissionItem waypoint(0, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT_INT, 0, 0, 0, qQNaN(),
                         coordinate.latitude(), coordinate.longitude(), 20, true, false);
    const auto original = SwarmMissionSnapshot::capture({&waypoint}, false);
    QVERIFY(original.validate(vehicle(), {}, 100).isEmpty());
    waypoint.setFrame(MAV_FRAME_GLOBAL_RELATIVE_ALT);
    const auto normalized = SwarmMissionSnapshot::capture({&waypoint}, false);
    QVERIFY(original.equivalent(normalized));
    QCOMPARE(original.fingerprint(), normalized.fingerprint());
    waypoint.setParam7(45);
    QVERIFY(!original.equivalent(SwarmMissionSnapshot::capture({&waypoint}, false)));
    SwarmConstraintChecker::Config limited;
    limited.maxAltitudeM = 3;
    QVERIFY(!original.validate(vehicle(), limited, 100).isEmpty());
    waypoint.setFrame(MAV_FRAME_GLOBAL_TERRAIN_ALT_INT);
    QVERIFY(!SwarmMissionSnapshot::capture({&waypoint}, false).validate(vehicle(), {}, 100).isEmpty());
    waypoint.setFrame(MAV_FRAME_GLOBAL_RELATIVE_ALT);
    waypoint.setCommand(MAV_CMD_DO_JUMP);
    QVERIFY(!SwarmMissionSnapshot::capture({&waypoint}, false).validate(vehicle(), {}, 100).isEmpty());
    waypoint.setCommand(MAV_CMD_NAV_RETURN_TO_LAUNCH);
    waypoint.setFrame(MAV_FRAME_MISSION);
    const auto returning = SwarmMissionSnapshot::capture({&waypoint}, false);
    QVERIFY2(returning.validate(vehicle(), {}, 100).isEmpty(), qPrintable(returning.validate(vehicle(), {}, 100)));
    QVERIFY(!returning.validate(vehicle(), limited, 100).isEmpty());
}

void SwarmSafetyTest::perceptionQueueMustRemainBounded()
{
    QVariantList batch;
    for (int i = 0; i < 10000; ++i)
        batch.append(QVariantMap{{"track_id", i}, {"lat", 0}, {"lon", 0}, {"confidence", .8}, {"time_usec", i + 1}});
    QVERIFY(QMetaObject::invokeMethod(_manager.get(), "_onTrackBatch", Qt::DirectConnection, Q_ARG(QVariantList, batch),
                                      Q_ARG(qint64, 1)));
    const auto diagnostics = _manager->perceptionDiagnostics();
    QVERIFY(diagnostics.value("pending_reports").toInt() <= 8192);
    QCOMPARE(diagnostics.value("dropped_reports").toInt(), 10000 - 8192);
    QTRY_COMPARE_WITH_TIMEOUT(_manager->perceptionDiagnostics().value("pending_reports").toInt(), 0, 5000);
    QVERIFY(_manager->perceptionDiagnostics().value("source_records").toInt() <= 8192);
    QVariantList invalid{QVariantMap{{"lat", qQNaN()}, {"lon", qInf()}}};
    QVERIFY(QMetaObject::invokeMethod(_manager.get(), "_onTrackBatch", Qt::DirectConnection,
                                      Q_ARG(QVariantList, invalid), Q_ARG(qint64, 2)));
    QCOMPARE(_manager->perceptionDiagnostics().value("rejected_coordinates").toInt(), 1);
}

void SwarmSafetyTest::terminalNotificationMustPreserveNewTask()
{
    QFETCH(int, notification);
    auto* task = _manager->taskController();
    QVERIFY(task->startTask("first", {{"gate", {}, "manual", {}}}, true));
    QTRY_VERIFY_WITH_TIMEOUT(task->waitingForProceed(), 15000);
    const auto firstId = task->taskId();
    QSignalSpy results(task, &SwarmTaskController::taskResultReady);
    bool started = false;
    const auto startNext = [&] {
        if (!task->running() && !started) {
            started = true;
            QVERIFY(task->startTask("second", {{"new gate", {}, "manual", {}}}, true));
        }
    };
    QMetaObject::Connection connection;
    if (notification == 0)
        connection = connect(task, &SwarmTaskController::taskChanged, task, startNext);
    else if (notification == 1)
        connection = connect(task, &SwarmTaskController::taskResultReady, task, startNext);
    else
        connection = connect(task, &SwarmTaskController::taskFinished, task, startNext);
    task->proceed();
    disconnect(connection);
    QVERIFY(started);
    QVERIFY(task->running());
    QVERIFY(task->taskId() != firstId);
    QCOMPARE(task->outcome(), SwarmTaskController::Outcome::None);
    QCOMPARE(results.size(), 1);
    QCOMPARE(results.first()[0].toString(), firstId);
    QCOMPARE(results.first()[1].toMap().value("outcome").toString(), "Succeeded");
    QFile file(QFileInfo(_configPath).absolutePath() + "/tasks.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto persisted = file.readAll();
    QVERIFY2(persisted.contains("second"),
             "Terminal taskChanged listener started second task, but old _finish erased its persisted record");
}

void SwarmSafetyTest::invalidCoordinateMustNotEnterCoverageMap()
{
    SwarmCOPModel cop;
    const double invalid = std::numeric_limits<double>::quiet_NaN();
    cop.ingestCoverage(invalid, invalid);
    QVERIFY2(cop.heatmap().isEmpty(), "Invalid GPS coordinates must not create geographic cells");
}

void SwarmSafetyTest::targetLeavingIncludeFenceMustRaiseAlert()
{
    SwarmEventDetector detector;
    detector.setFences({{{30, 120}, {30, 120.01}, {30.01, 120.01}, {30.01, 120}}}, {});
    SwarmTrackFusion::FusedTrack track;
    track.fusedId = 1;
    track.state = "confirmed";
    track.lat = 30.005;
    track.lon = 120.005;
    QVERIFY(detector.checkTracks({track}, 1000).isEmpty());
    track.lon = 120.02;
    const auto alerts = detector.checkTracks({track}, 2000);
    QVERIFY2(!alerts.isEmpty(), "Documented target leaving permitted area does not raise a boundary alert");
}

void SwarmSafetyTest::continuousTrackHistoryMustExpire()
{
    SwarmTrackFusion fusion;
    SwarmTrackFusion::TrackReport report;
    report.lat = 30;
    report.lon = 120;
    report.confidence = 0.8;
    for (int i = 0; i < 30000; ++i) {
        report.trackId = i;
        report.timeUsec = (i + 1LL) * 500000;
        fusion.ingest(report, i * 500LL);
        fusion.prune(i * 500LL);
    }
    QCOMPARE(fusion.allTracks().size(), 1);
    QVERIFY(fusion.allTracks().first().sourceTrackIds.size() <= 64);
}

UT_REGISTER_TEST(SwarmSafetyTest, TestLabel::Integration, TestLabel::Vehicle)
