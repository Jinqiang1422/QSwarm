#include "SwarmRegressionTest.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopeGuard>
#include <QtCore/QTimer>
#include <QtNetwork/QUdpSocket>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <algorithm>
#include <cmath>

#include "Fact.h"
#include "FirmwarePlugin.h"
#include "FormationController.h"
#include "MissionItem.h"
#include "MissionManager.h"
#include "MultiVehicleManager.h"
#include "PX4FirmwarePlugin.h"
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
#include "px4_custom_mode.h"

void SwarmRegressionTest::px4FlightModesRetainMeaningAcrossProtocolVersions()
{
    PX4FirmwarePlugin plugin;
    constexpr uint32_t externalMode = 0x7f040000;
    FlightModeList modes{
        {"Land", 7, PX4CustomMode::AUTO_MISSION, true, true}, {"Mission", 6, PX4CustomMode::AUTO_RTL, true, true},
        {"Takeoff", 8, PX4CustomMode::AUTO_LAND, true, true}, {"", 9, PX4CustomMode::AUTO_TAKEOFF, false, true},
        {"External survey", 0, externalMode, true, true},
    };
    plugin.updateAvailableFlightModes(modes);
    QCOMPARE(plugin.flightMode(MAV_MODE_FLAG_CUSTOM_MODE_ENABLED, PX4CustomMode::AUTO_MISSION), "Mission");
    QCOMPARE(plugin.flightMode(MAV_MODE_FLAG_CUSTOM_MODE_ENABLED, PX4CustomMode::AUTO_RTL), "Return");
    QCOMPARE(plugin.flightMode(MAV_MODE_FLAG_CUSTOM_MODE_ENABLED, PX4CustomMode::AUTO_LAND), "Land");
    QCOMPARE(plugin.flightMode(MAV_MODE_FLAG_CUSTOM_MODE_ENABLED, PX4CustomMode::AUTO_TAKEOFF), "Takeoff");
    uint8_t baseMode = 0;
    uint32_t customMode = 0;
    QVERIFY(plugin.setFlightMode("Mission", &baseMode, &customMode));
    QCOMPARE(customMode, uint32_t(PX4CustomMode::AUTO_MISSION));
    modes.last().mode_name = "Renamed external survey";
    modes.first().mode_name = "Land";
    plugin.updateAvailableFlightModes(modes);
    QCOMPARE(plugin.flightMode(MAV_MODE_FLAG_CUSTOM_MODE_ENABLED, PX4CustomMode::AUTO_MISSION), "Mission");
    QCOMPARE(plugin.flightMode(MAV_MODE_FLAG_CUSTOM_MODE_ENABLED, externalMode), "Renamed external survey");
}

void SwarmRegressionTest::activeFormationEditsUpdateTargets()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(4);
    auto* mvm = MultiVehicleManager::instance();
    QList<Vehicle*> followers;
    QSet<int> members;
    for (int i = 0; i < mvm->vehicles()->count(); ++i) {
        auto* member = qobject_cast<Vehicle*>(mvm->vehicles()->get(i));
        QVERIFY(member);
        member->sendMavCommand(member->defaultComponentId(), MAV_CMD_NAV_TAKEOFF, true, 0, 0, 0, 0, 0, 0, 10);
        members.insert(member->id());
        if (member != vehicle())
            followers.append(member);
    }
    QTRY_VERIFY_WITH_TIMEOUT(
        std::all_of(followers.begin(), followers.end(),
                    [](Vehicle* v) { return v->armed() && v->altitudeRelative()->rawValue().toDouble() >= 9; }) &&
            vehicle()->armed() && vehicle()->altitudeRelative()->rawValue().toDouble() >= 9,
        TestTimeout::longMs());
    std::sort(followers.begin(), followers.end(), [](Vehicle* a, Vehicle* b) { return a->id() < b->id(); });
    const double formationHeading = 90;
    qint64 now = 1000;
    SwarmClock::setTestClock([&now] { return now; });
    const auto resetClock = qScopeGuard([] { SwarmClock::setTestClock({}); });
    FormationController formation;
    formation.setMembers(members);
    QHash<int, QGeoCoordinate> targets;
    bool allowTargets = true;
    int pendingCommands = 0;
    for (auto* follower : followers) {
        connect(follower, &Vehicle::mavCommandResult, &formation, [&](int, int, int command, int, int) {
            if (command == MAV_CMD_DO_REPOSITION)
                --pendingCommands;
        });
    }
    const auto waitCommands = [&] { QTRY_COMPARE_WITH_TIMEOUT(pendingCommands, 0, TestTimeout::longMs()); };
    formation.setTargetAllowedCallback([&](int id, const QGeoCoordinate& coordinate) {
        targets[id] = coordinate;
        if (allowTargets)
            ++pendingCommands;
        return allowTargets;
    });
    const auto verifyMeasuredTargets = [&] {
        waitCommands();
        QCOMPARE(targets.size(), 4);
        for (auto* follower : followers) {
            const auto target = targets.value(follower->id());
            QVERIFY(target.distanceTo(follower->coordinate()) < 0.01);
            QCOMPARE(target.altitude(), follower->altitudeAMSL()->rawValue().toDouble());
        }
    };
    // MockLink varies yaw in its telemetry; use a fixed reference for convergence assertions. A leader
    // moving faster than 1 m/s skips the synchronized reshape leg, so this test covers station keeping.
    const auto driveLeader = [&] {
        vehicle()->heading()->setRawValue(formationHeading);
        setLeaderGroundSpeed(vehicle(), 2.0);
    };
    const auto tick = [&](qint64 milliseconds = 1000) {
        now += milliseconds;
        driveLeader();
        QVERIFY(QMetaObject::invokeMethod(&formation, "_updateFormation", Qt::DirectConnection));
        waitCommands();
    };
    driveLeader();
    QVERIFY(formation.start(vehicle()->id()));
    verifyMeasuredTargets();
    const QList<FormationController::FormationType> shapes{
        FormationController::FormationColumn, FormationController::FormationLine, FormationController::FormationCircle,
        FormationController::FormationWedge};
    for (auto shape : shapes) {
        // Mock aircraft stay at their measured positions while targets move ahead. Each subsequent
        // switch must discard those unreached targets, including changes midway through a transition.
        formation.setFormationType(shape);
        tick();
        verifyMeasuredTargets();
        auto previous = targets;
        for (int step = 0; step < 3; ++step) {
            tick();
            for (auto* follower : followers) {
                const auto target = targets.value(follower->id());
                QVERIFY(target.distanceTo(previous.value(follower->id())) <= 2.01);
                QVERIFY(qAbs(target.altitude() - previous.value(follower->id()).altitude()) <= 1.01);
            }
            previous = targets;
        }
        QVERIFY(targets.value(followers.first()->id()).distanceTo(followers.first()->coordinate()) > 0.1);
    }

    formation.setSpacingMeters(20);
    tick();
    verifyMeasuredTargets();
    formation.setVerticalStepMeters(6);
    tick();
    verifyMeasuredTargets();
    // Clock jumps (e.g. a busy UI) must not yield a large target jump.
    const auto beforeDelay = targets;
    tick(60000);
    for (auto* follower : followers) {
        QVERIFY(targets.value(follower->id()).distanceTo(beforeDelay.value(follower->id())) <= 2.01);
    }
    // Rejected targets must not advance the transition behind the constraint checker.
    allowTargets = false;
    tick();
    const auto rejected = targets;
    for (int step = 0; step < 5; ++step)
        tick();
    for (auto* follower : followers)
        QCOMPARE(targets.value(follower->id()), rejected.value(follower->id()));
    allowTargets = true;
    int transitionTicks = 0;
    QHash<int, QGeoCoordinate> expectedTargets;
    for (int i = 0; i < followers.size(); ++i) {
        const int rank = (i + 2) / 2;
        const int side = i % 2 == 0 ? -1 : 1;
        const auto expected =
            vehicle()->coordinate().atDistanceAndAzimuth(20 * rank, formationHeading + 180 + side * 30);
        auto destination = expected;
        destination.setAltitude(vehicle()->altitudeAMSL()->rawValue().toDouble() + 6 * rank);
        expectedTargets.insert(followers[i]->id(), destination);
        transitionTicks =
            qMax(transitionTicks, int(std::ceil(followers[i]->coordinate().distanceTo(destination) / 2)) + 2);
        transitionTicks = qMax(
            transitionTicks,
            int(std::ceil(std::abs(destination.altitude() - followers[i]->altitudeAMSL()->rawValue().toDouble()))) + 2);
    }
    for (int step = 0; step < transitionTicks; ++step)
        tick();
    for (auto* follower : followers) {
        const auto expected = expectedTargets.value(follower->id());
        QVERIFY2(targets.value(follower->id()).distanceTo(expected) < 0.01,
                 qPrintable(QStringLiteral("Remaining target distance: %1 m")
                                .arg(targets.value(follower->id()).distanceTo(expected))));
        QCOMPARE(targets.value(follower->id()).altitude(), expected.altitude());
    }

    // A subsequent shape change must use new telemetry, not the position at the first start.
    Vehicle* movedFollower = mvm->getVehicleById(_extraLinks.first()->vehicleId());
    QVERIFY(movedFollower);
    const auto moved = movedFollower->coordinate().atDistanceAndAzimuth(25, 65);
    mavlink_message_t position{};
    mavlink_msg_global_position_int_pack(movedFollower->id(), movedFollower->defaultComponentId(), &position, 0,
                                         qRound(moved.latitude() * 1e7), qRound(moved.longitude() * 1e7),
                                         qRound(movedFollower->altitudeAMSL()->rawValue().toDouble() * 1000), 10000, 0,
                                         0, 0, 0);
    _extraLinks.first()->respondWithMavlinkMessage(position);
    QTRY_VERIFY_WITH_TIMEOUT(movedFollower->coordinate().distanceTo(moved) < 0.1, TestTimeout::longMs());
    movedFollower->altitudeAMSL()->setRawValue(movedFollower->altitudeAMSL()->rawValue().toDouble() + 4);
    const double movedAltitude = movedFollower->altitudeAMSL()->rawValue().toDouble();
    formation.setFormationType(FormationController::FormationLine);
    now += 1000;
    driveLeader();
    QVERIFY(QMetaObject::invokeMethod(&formation, "_updateFormation", Qt::DirectConnection));
    QVERIFY(targets.value(movedFollower->id()).distanceTo(moved) < 0.1);
    QCOMPARE(targets.value(movedFollower->id()).altitude(), movedAltitude);
    waitCommands();
    formation.stop();
    targets.clear();
    tick();
    QVERIFY(targets.isEmpty());
    driveLeader();
    QVERIFY(formation.start(vehicle()->id()));
    verifyMeasuredTargets();
}

void SwarmRegressionTest::guidanceTargetTracksSlotMotion()
{
    const QGeoCoordinate origin(47.397, 8.5455, 500);

    // The leader flies north at 10 m/s, e.g. after a map "go to location". A target on its slot must
    // move with the slot instead of being pinned by the 2 m/s catch-up cap.
    QGeoCoordinate slot = origin;
    QGeoCoordinate target = origin;
    for (int step = 0; step < 4; ++step) {
        QGeoCoordinate next = slot.atDistanceAndAzimuth(10, 0);
        next.setAltitude(slot.altitude() + 3);
        target = FormationController::advanceGuidanceTarget(target, slot, next, 1.0);
        QVERIFY2(target.distanceTo(next) < 0.01,
                 qPrintable(QStringLiteral("target is %1 m from a slot that moved 10 m").arg(target.distanceTo(next))));
        QVERIFY(qAbs(target.altitude() - next.altitude()) < 0.01);
        slot = next;
    }

    // A target still converging is carried along with the slot, then closes at most 2 m of the
    // remaining horizontal gap and 1 m of the vertical gap per second.
    QGeoCoordinate lagging = origin.atDistanceAndAzimuth(20, 270);
    lagging.setAltitude(origin.altitude() - 5);
    QGeoCoordinate moved = origin.atDistanceAndAzimuth(10, 0);
    moved.setAltitude(origin.altitude());
    const QGeoCoordinate carried = lagging.atDistanceAndAzimuth(10, 0);
    const QGeoCoordinate advanced = FormationController::advanceGuidanceTarget(lagging, origin, moved, 1.0);
    QVERIFY(qAbs(advanced.distanceTo(carried) - 2.0) < 0.01);
    QVERIFY(qAbs(advanced.distanceTo(moved) - (carried.distanceTo(moved) - 2.0)) < 0.01);
    QVERIFY(qAbs(advanced.altitude() - (origin.altitude() - 4)) < 0.01);

    // The first cycle has no previous slot position and starts from the measured position.
    const QGeoCoordinate first = FormationController::advanceGuidanceTarget(lagging, QGeoCoordinate(), moved, 1.0);
    QVERIFY(qAbs(first.distanceTo(lagging) - 2.0) < 0.01);
}

void SwarmRegressionTest::formationSkipsFollowerAwaitingRepositionAck()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(1);
    Vehicle* follower = MultiVehicleManager::instance()->getVehicleById(_extraLinks.first()->vehicleId());
    QVERIFY(follower);
    for (Vehicle* member : {vehicle(), follower})
        member->sendMavCommand(member->defaultComponentId(), MAV_CMD_NAV_TAKEOFF, true, 0, 0, 0, 0, 0, 0, 10);
    QTRY_VERIFY_WITH_TIMEOUT(follower->armed() && follower->altitudeRelative()->rawValue().toDouble() >= 9 &&
                                 vehicle()->armed() && vehicle()->altitudeRelative()->rawValue().toDouble() >= 9,
                             TestTimeout::longMs());

    FormationController formation;
    formation.setMembers({vehicle()->id(), follower->id()});
    int commandedCycles = 0;
    formation.setTargetAllowedCallback([&](int, const QGeoCoordinate&) {
        ++commandedCycles;
        return true;
    });
    // A moving leader skips the synchronized reshape leg; this test covers station keeping.
    setLeaderGroundSpeed(vehicle(), 2.0);
    // start() runs one cycle immediately. Its reposition ACK is not processed until the event loop runs.
    QVERIFY(formation.start(vehicle()->id()));
    QCOMPARE(commandedCycles, 1);
    QVERIFY(follower->isMavCommandPending(follower->defaultComponentId(), MAV_CMD_DO_REPOSITION));
    // The next cycle must wait for that ACK instead of sending a duplicate the command queue rejects.
    QVERIFY(QMetaObject::invokeMethod(&formation, "_updateFormation", Qt::DirectConnection));
    QCOMPARE(commandedCycles, 1);

    QTRY_VERIFY_WITH_TIMEOUT(!follower->isMavCommandPending(follower->defaultComponentId(), MAV_CMD_DO_REPOSITION),
                             TestTimeout::longMs());
    const int beforeCycle = commandedCycles;
    QVERIFY(QMetaObject::invokeMethod(&formation, "_updateFormation", Qt::DirectConnection));
    QCOMPARE(commandedCycles, beforeCycle + 1);
    formation.stop();
}

void SwarmRegressionTest::formationRebuildsSlotsWhenMemberLacksOne()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(2);
    auto* mvm = MultiVehicleManager::instance();
    QList<Vehicle*> followers;
    QSet<int> memberIds;
    for (int i = 0; i < mvm->vehicles()->count(); ++i) {
        auto* member = qobject_cast<Vehicle*>(mvm->vehicles()->get(i));
        QVERIFY(member);
        member->sendMavCommand(member->defaultComponentId(), MAV_CMD_NAV_TAKEOFF, true, 0, 0, 0, 0, 0, 0, 10);
        memberIds.insert(member->id());
        if (member != vehicle())
            followers.append(member);
    }
    QTRY_VERIFY_WITH_TIMEOUT(
        std::all_of(followers.begin(), followers.end(),
                    [](Vehicle* v) { return v->armed() && v->altitudeRelative()->rawValue().toDouble() >= 9; }) &&
            vehicle()->armed() && vehicle()->altitudeRelative()->rawValue().toDouble() >= 9,
        TestTimeout::longMs());
    std::sort(followers.begin(), followers.end(), [](Vehicle* a, Vehicle* b) { return a->id() < b->id(); });

    qint64 now = 1000;
    SwarmClock::setTestClock([&now] { return now; });
    const auto resetClock = qScopeGuard([] { SwarmClock::setTestClock({}); });
    FormationController formation;
    formation.setMembers(QSet<int>{vehicle()->id(), followers.first()->id()});
    QHash<int, QGeoCoordinate> targets;
    int pendingCommands = 0;
    for (auto* follower : followers) {
        connect(follower, &Vehicle::mavCommandResult, &formation, [&](int, int, int command, int, int) {
            if (command == MAV_CMD_DO_REPOSITION)
                --pendingCommands;
        });
    }
    formation.setTargetAllowedCallback([&](int id, const QGeoCoordinate& coordinate) {
        targets[id] = coordinate;
        ++pendingCommands;
        return true;
    });
    const auto tick = [&] {
        now += 1000;
        vehicle()->heading()->setRawValue(90);
        QVERIFY(QMetaObject::invokeMethod(&formation, "_updateFormation", Qt::DirectConnection));
        QTRY_COMPARE_WITH_TIMEOUT(pendingCommands, 0, TestTimeout::longMs());
    };
    // A moving leader skips the synchronized reshape leg; this test covers station keeping.
    setLeaderGroundSpeed(vehicle(), 2.0);
    QVERIFY(formation.start(vehicle()->id()));
    for (int i = 0; i < 15; ++i)
        tick();
    QVERIFY(targets.contains(followers.first()->id()));
    QVERIFY(!targets.contains(followers.last()->id()));

    // Membership refresh while active clears all slots: the same stranded state a handover leaves
    // when a member's coordinate was momentarily invalid. The next cycle must heal it.
    targets.clear();
    formation.setMembers(memberIds);
    tick();
    QVERIFY2(targets.contains(followers.first()->id()), "existing member must keep receiving commands");
    QVERIFY2(targets.contains(followers.last()->id()), "added member must start receiving commands");
    formation.stop();
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

void SwarmRegressionTest::coverageLeavesHeadingToVehicle()
{
    // PX4 flies takeoff/waypoint param4 as an absolute yaw, so 0 turns every aircraft north. NaN keeps
    // its heading mode: face the next waypoint, and hold the heading on reaching the last one.
    // ArduCopter does not store the field and reads it back as 0, so it must stay 0 there.
    const QGeoCoordinate here = vehicle()->coordinate();
    const auto items = _manager->missionPlanner()->buildCoverageMissionItems(vehicle(), here, here, 60, 30, 0, -15, 15);
    const auto release = qScopeGuard([&items] { qDeleteAll(items); });
    int flownItems = 0;
    for (int i = 1; i < items.size(); ++i) {  // Item 0 is the Home placeholder.
        const MissionItem* item = items[i];
        if (item->command() != MAV_CMD_NAV_WAYPOINT && item->command() != MAV_CMD_NAV_TAKEOFF)
            continue;
        ++flownItems;
        const QString context =
            QStringLiteral("item %1 (command %2) yaw %3").arg(i).arg(item->command()).arg(item->param4());
        if (vehicle()->px4Firmware())
            QVERIFY2(std::isnan(item->param4()), qPrintable(context));
        else
            QVERIFY2(item->param4() == 0.0, qPrintable(context));
    }
    QVERIFY(flownItems > 2);
}

void SwarmRegressionTest::coverageLeavesHeadingToVehicle_data()
{
    TestData::addAutopilotRows();
}

void SwarmRegressionTest::missionCompletionRequiresCurrentExecutionEvidence()
{
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->homePosition().isValid(), TestTimeout::longMs());
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
    // Deliver the queued execution completion before advancing the simulated stage clock.
    QCoreApplication::sendPostedEvents(task, QEvent::MetaCall);
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

void SwarmRegressionTest::missionReplacementBlocksLaterStart_data()
{
    QTest::addColumn<bool>("replace");
    QTest::newRow("replacement") << true;
    QTest::newRow("same-content-readback") << false;
}

void SwarmRegressionTest::missionReplacementBlocksLaterStart()
{
    QFETCH(bool, replace);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->homePosition().isValid(), TestTimeout::longMs());
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
    if (replace) {
        _manager->missionPlanner()->setSurveyAltitude(45);
        const auto items = _manager->missionPlanner()->buildCoverageMissionItems(
            vehicle(), vehicle()->homePosition(), vehicle()->coordinate(), 30, 30, 0, -15, 15);
        QSignalSpy sent(mission, &MissionManager::sendComplete);
        mission->writeMissionItems(items);
        QTRY_VERIFY_WITH_TIMEOUT(!sent.isEmpty(), TestTimeout::longMs());
        QVERIFY(!sent.first()[0].toBool());
    }
    QSignalSpy read(mission, &MissionManager::newMissionItemsAvailable);
    mission->loadFromVehicle();
    QTRY_VERIFY_WITH_TIMEOUT(!read.isEmpty(), TestTimeout::longMs());
    mockLink()->clearReceivedMavCommandCounts();
    QSignalSpy executed(_manager.get(), &SwarmManager::executionFinished);
    task->proceed();
    QTRY_VERIFY_WITH_TIMEOUT(!executed.isEmpty(), TestTimeout::longMs());
    QCOMPARE(task->blocked(), replace);
    QCOMPARE(executed.last()[1].toBool(), !replace);
    if (replace) {
        QVERIFY(!vehicle()->armed());
        QCOMPARE(mockLink()->receivedMavCommandCount(MAV_CMD_COMPONENT_ARM_DISARM), 0);
    }
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
