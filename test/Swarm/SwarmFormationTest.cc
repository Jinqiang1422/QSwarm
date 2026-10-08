#include "SwarmFormationTest.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtTest/QSignalSpy>
#include <algorithm>
#include <cmath>

#include "FirmwarePlugin.h"
#include "FlyViewSettings.h"
#include "FormationController.h"
#include "MissionItem.h"
#include "MissionManager.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "SwarmManager.h"
#include "Vehicle.h"
#include "VehicleFactGroup.h"

namespace {

/// Makes vehicle report a failsafe the way PX4 does: HEARTBEAT system_status = MAV_STATE_CRITICAL with the
/// flight mode unchanged. The message reaches the Vehicle synchronously; MockLink's own heartbeat restores the
/// state about a second later, so callers assert right after it lands without returning to the event loop.
void reportFailsafe(MockLink* link, Vehicle* vehicle)
{
    mavlink_message_t heartbeat{};
    (void) mavlink_msg_heartbeat_pack_chan(vehicle->id(), vehicle->defaultComponentId(), link->outgoingMavlinkChannel(),
                                           &heartbeat, vehicle->vehicleType(), vehicle->firmwareType(),
                                           vehicle->baseMode(), vehicle->customMode(), MAV_STATE_CRITICAL);
    link->respondWithMavlinkMessage(heartbeat);
}

MockLink* linkForVehicle(const QList<QPointer<MockLink>>& links, int vehicleId)
{
    for (const QPointer<MockLink>& link : links) {
        if (link && link->vehicleId() == vehicleId)
            return link;
    }
    return nullptr;
}

QSet<int> transitMemberIds(const FormationController& formation)
{
    QSet<int> ids;
    const QVariantList members = formation.transitStatus().value(QStringLiteral("members")).toList();
    for (const QVariant& member : members)
        ids.insert(member.toMap().value(QStringLiteral("id")).toInt());
    return ids;
}

/// Target of the last native (operator) goto the link received. MockLink does not advertise COMMAND_INT, so a
/// native goto arrives as a COMMAND_LONG DO_REPOSITION, while formation commands always use COMMAND_INT.
QGeoCoordinate nativeGotoTarget(MockLink* link)
{
    mavlink_message_t message{};
    if (!link || !link->lastReceivedMavlinkMessage(MAVLINK_MSG_ID_COMMAND_LONG, message))
        return QGeoCoordinate();
    mavlink_command_long_t command{};
    mavlink_msg_command_long_decode(&message, &command);
    return command.command == MAV_CMD_DO_REPOSITION ? QGeoCoordinate(command.param5, command.param6) : QGeoCoordinate();
}

/// Synchronized legs check every member's path against that member's own home altitude and refuse members
/// without a home. MockLink first reports HOME_POSITION about 10 s after it connects.
bool everyHomeKnown()
{
    QmlObjectListModel* vehicles = MultiVehicleManager::instance()->vehicles();
    for (int i = 0; i < vehicles->count(); ++i) {
        auto* member = qobject_cast<Vehicle*>(vehicles->get(i));
        if (!member || !member->homePosition().isValid())
            return false;
    }
    return true;
}

}  // namespace

void SwarmFormationTest::operatorGotoMovesWholeFormation()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(2);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    auto* mvm = MultiVehicleManager::instance();
    QHash<int, MockLink*> links{{vehicle()->id(), mockLink()}};
    for (const QPointer<MockLink>& link : std::as_const(_extraLinks))
        links.insert(link->vehicleId(), link);
    for (MockLink* link : std::as_const(links))
        link->setRepositionSupported(true);
    QTRY_VERIFY_WITH_TIMEOUT(everyHomeKnown(), TestTimeout::longMs());
    // MockLink places later vehicles north-east of the leader. A formation facing south-west keeps every
    // synchronized path clear of the others, so each goto below is planned as a single leg.
    vehicle()->heading()->setRawValue(214);
    QVERIFY(_manager->startFormation(vehicle()->id()));
    QSignalSpy feedback(_manager.get(), &SwarmManager::swarmCommandFeedback);

    // Like the native goto, a goal farther from the leader than the go-to distance limit is refused outright.
    const double maxGotoDistance =
        SettingsManager::instance()->flyViewSettings()->maxGoToLocationDistance()->rawValue().toDouble();
    QVERIFY(!_manager->gotoLocation(followers.last(),
                                    vehicle()->coordinate().atDistanceAndAzimuth(maxGotoDistance + 100, 214)));
    QVERIFY(!feedback.isEmpty());
    QVERIFY2(feedback.last().first().toString().contains(QStringLiteral("目标点过远")),
             qPrintable(feedback.last().first().toString()));
    QVERIFY(_manager->formation()->transitStatus().value(QStringLiteral("kind")).toString() != QStringLiteral("goto"));

    // A goto on any member moves the whole formation: every member, the leader included, is commanded in
    // the same cycle with a ground speed that makes all of them arrive together.
    const auto verifySynchronizedGoto = [&](Vehicle* target, const QGeoCoordinate& goal) {
        QTRY_VERIFY_WITH_TIMEOUT(std::none_of(links.cbegin(), links.cend(),
                                              [mvm](MockLink* link) {
                                                  Vehicle* member = mvm->getVehicleById(link->vehicleId());
                                                  return member &&
                                                         member->isMavCommandPending(member->defaultComponentId(),
                                                                                     MAV_CMD_DO_REPOSITION);
                                              }),
                                 TestTimeout::longMs());
        QHash<int, QGeoCoordinate> starts;
        for (MockLink* link : std::as_const(links)) {
            link->clearReceivedMavlinkMessageCounts();
            starts.insert(link->vehicleId(), mvm->getVehicleById(link->vehicleId())->coordinate());
        }
        feedback.clear();
        QVERIFY(_manager->gotoLocation(target, goal));
        QVERIFY(_manager->formation()->transitActive());
        QCOMPARE(_manager->formation()->transitLegCount(), 1);
        QVERIFY(std::any_of(feedback.cbegin(), feedback.cend(), [](const QList<QVariant>& arguments) {
            return arguments.first().toString().contains(QStringLiteral("编队整体飞往目标点"));
        }));
        QTRY_VERIFY_WITH_TIMEOUT(std::all_of(links.cbegin(), links.cend(),
                                             [](MockLink* link) { return lastFormationReposition(link).valid(); }),
                                 TestTimeout::longMs());
        QVERIFY(lastFormationReposition(mockLink()).target.distanceTo(goal) < 1.0);
        QList<double> arrivalSeconds;
        for (MockLink* link : std::as_const(links)) {
            const Reposition command = lastFormationReposition(link);
            QVERIFY(command.speed > 0);
            arrivalSeconds.append(starts.value(link->vehicleId()).distanceTo(command.target) / command.speed);
        }
        const auto [earliest, latest] = std::minmax_element(arrivalSeconds.cbegin(), arrivalSeconds.cend());
        QVERIFY2(*latest - *earliest < 0.5, qPrintable(QStringLiteral("arrival spread %1 s").arg(*latest - *earliest)));
    };
    verifySynchronizedGoto(followers.last(), vehicle()->coordinate().atDistanceAndAzimuth(150, 214));
    if (QTest::currentTestFailed())
        return;
    // A second goto, issued on the leader, replaces the leg in progress.
    verifySynchronizedGoto(vehicle(), vehicle()->coordinate().atDistanceAndAzimuth(100, 214));
    if (QTest::currentTestFailed())
        return;

    // Without an active formation the goto applies to the selected vehicle only. MockLink does not
    // advertise COMMAND_INT, so this native goto arrives as COMMAND_LONG.
    Vehicle* follower = followers.last();
    MockLink* followerLink = links.value(follower->id());
    _manager->stopFormation();
    QVERIFY(!_manager->formation()->transitActive());
    QTRY_VERIFY_WITH_TIMEOUT(!follower->isMavCommandPending(follower->defaultComponentId(), MAV_CMD_DO_REPOSITION),
                             TestTimeout::longMs());
    const QGeoCoordinate soloGoal = follower->coordinate().atDistanceAndAzimuth(80, 180);
    QVERIFY(_manager->gotoLocation(follower, soloGoal));
    QTRY_VERIFY_WITH_TIMEOUT(
        nativeGotoTarget(followerLink).isValid() && nativeGotoTarget(followerLink).distanceTo(soloGoal) < 1.0,
        TestTimeout::longMs());
}

void SwarmFormationTest::formationRouteFromLeaderMission()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(2);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    mockLink()->setRepositionSupported(true);
    for (const QPointer<MockLink>& link : std::as_const(_extraLinks))
        link->setRepositionSupported(true);
    QTRY_VERIFY_WITH_TIMEOUT(everyHomeKnown(), TestTimeout::longMs());
    const QGeoCoordinate home = vehicle()->homePosition();
    const auto uploadRoute = [this, &home](const QList<std::pair<MAV_CMD, QGeoCoordinate>>& route) {
        // Index 0 is the Home placeholder that PlanManager strips for PX4.
        QList<MissionItem*> items{new MissionItem(0, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL, 0, 0, 0, 0,
                                                  home.latitude(), home.longitude(), home.altitude(), true, false)};
        for (const auto& [command, point] : route) {
            items.append(new MissionItem(items.size(), command, MAV_FRAME_GLOBAL_RELATIVE_ALT, 0, 0, 0, NAN,
                                         point.latitude(), point.longitude(), point.altitude(), true, false));
        }
        auto* mission = vehicle()->missionManager();
        QSignalSpy sent(mission, &MissionManager::sendComplete);
        mission->writeMissionItems(items);
        QTRY_VERIFY_WITH_TIMEOUT(!sent.isEmpty(), TestTimeout::longMs());
        QVERIFY(!sent.first().first().toBool());
        QSignalSpy read(mission, &MissionManager::newMissionItemsAvailable);
        mission->loadFromVehicle();
        QTRY_VERIFY_WITH_TIMEOUT(!read.isEmpty(), TestTimeout::longMs());
    };
    // Relative altitudes: 120 m south-west, then 120 m north-west, then a landing the formation leaves alone.
    QGeoCoordinate first = vehicle()->coordinate().atDistanceAndAzimuth(120, 214);
    first.setAltitude(25);
    QGeoCoordinate second = first.atDistanceAndAzimuth(120, 304);
    second.setAltitude(30);
    QGeoCoordinate landing = second;
    landing.setAltitude(0);
    uploadRoute({{MAV_CMD_NAV_WAYPOINT, first}, {MAV_CMD_NAV_WAYPOINT, second}, {MAV_CMD_NAV_LAND, landing}});
    if (QTest::currentTestFailed())
        return;
    vehicle()->heading()->setRawValue(214);
    QVERIFY(_manager->startFormation(vehicle()->id()));
    QSignalSpy reconfirmFeedback(_manager.get(), &SwarmManager::swarmCommandFeedback);
    const auto lastFeedback = [&reconfirmFeedback] {
        return reconfirmFeedback.isEmpty() ? QString() : reconfirmFeedback.last().first().toString();
    };
    const QString needsConfirm = QStringLiteral("请先查看并确认航线");
    const QString reconfirm = QStringLiteral("长机或航线已变化，请重新确认");
    const auto routeRunning = [this] {
        return _manager->formation()->transitStatus().value(QStringLiteral("kind")).toString() ==
               QStringLiteral("route");
    };

    // Starting needs the confirmation the operator was shown; there is none yet.
    QVERIFY(!_manager->formationRouteConfirmable());
    QVERIFY(!_manager->startFormationRoute());
    QVERIFY2(lastFeedback().contains(needsConfirm), qPrintable(lastFeedback()));
    QVERIFY(!routeRunning());

    const QString summary = _manager->formationRouteSummary();
    QVERIFY2(summary.contains(QStringLiteral("2 个航点")) && summary.contains(QStringLiteral("末尾降落")),
             qPrintable(summary));
    // An executable summary records a confirmation the panel's "确认" button relies on.
    QVERIFY(_manager->formationRouteConfirmable());
    // A route uploaded after the confirmation is not the route the operator confirmed.
    QGeoCoordinate higherSecond = second;
    higherSecond.setAltitude(35);
    uploadRoute({{MAV_CMD_NAV_WAYPOINT, first}, {MAV_CMD_NAV_WAYPOINT, higherSecond}, {MAV_CMD_NAV_LAND, landing}});
    if (QTest::currentTestFailed())
        return;
    QVERIFY(!_manager->startFormationRoute());
    QVERIFY2(lastFeedback().contains(reconfirm), qPrintable(lastFeedback()));
    QVERIFY(!routeRunning());
    // The attempt consumed the (now stale) confirmation regardless of the outcome.
    QVERIFY(!_manager->formationRouteConfirmable());

    // Confirmed afresh, the route starts. Its first leg takes the leader straight to the first waypoint at that
    // waypoint's altitude (relative altitudes are converted against the leader's home).
    const QString freshSummary = _manager->formationRouteSummary();
    QVERIFY2(freshSummary.contains(QStringLiteral("2 个航点")), qPrintable(freshSummary));
    QVERIFY(_manager->formationRouteConfirmable());
    mockLink()->clearReceivedMavlinkMessageCounts();
    QVERIFY(_manager->startFormationRoute());
    QVERIFY(!_manager->formationRouteConfirmable());
    QVERIFY(routeRunning());
    QVERIFY(_manager->formation()->transitLegCount() >= 2);
    QTRY_VERIFY_WITH_TIMEOUT(lastFormationReposition(mockLink()).valid(), TestTimeout::longMs());
    const Reposition leaderLeg = lastFormationReposition(mockLink());
    QVERIFY2(leaderLeg.target.distanceTo(first) < 1.0,
             qPrintable(QStringLiteral("first leg ends %1 m from the first waypoint, %2 legs planned")
                            .arg(leaderLeg.target.distanceTo(first))
                            .arg(_manager->formation()->transitLegCount())));
    QVERIFY2(qAbs(leaderLeg.target.altitude() - (home.altitude() + 25)) < 0.5,
             qPrintable(QStringLiteral("first leg altitude %1 m AMSL, home %2 m")
                            .arg(leaderLeg.target.altitude())
                            .arg(home.altitude())));

    // Any item other than a waypoint (apart from a leading takeoff and a trailing landing) rejects the route.
    _manager->stopFormation();
    uploadRoute({{MAV_CMD_NAV_WAYPOINT, first}, {MAV_CMD_DO_CHANGE_SPEED, first}, {MAV_CMD_NAV_WAYPOINT, second}});
    if (QTest::currentTestFailed())
        return;
    QVERIFY(_manager->startFormation(vehicle()->id()));
    const QString unsupportedSummary = _manager->formationRouteSummary();
    QVERIFY2(unsupportedSummary.contains(QStringLiteral("无法按航线飞行")), qPrintable(unsupportedSummary));
    // A failed summary clears whatever confirmation was pending, so there is nothing left to start.
    QVERIFY(!_manager->formationRouteConfirmable());
    QSignalSpy feedback(_manager.get(), &SwarmManager::swarmCommandFeedback);
    QVERIFY(!_manager->startFormationRoute());
    QVERIFY(_manager->formation()->transitStatus().value(QStringLiteral("kind")).toString() != QStringLiteral("route"));
    QVERIFY(!feedback.isEmpty());
    QVERIFY2(feedback.last().first().toString().contains(QStringLiteral("不支持")),
             qPrintable(feedback.last().first().toString()));
}

void SwarmFormationTest::formationGotoRefusedOutsideFence()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(1);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    mockLink()->setRepositionSupported(true);
    for (const QPointer<MockLink>& link : std::as_const(_extraLinks))
        link->setRepositionSupported(true);
    QTRY_VERIFY_WITH_TIMEOUT(everyHomeKnown(), TestTimeout::longMs());
    vehicle()->heading()->setRawValue(214);
    QVERIFY(_manager->startFormation(vehicle()->id()));

    // An exclusion zone around the goal, far from every member's current position. The fixture wrote an
    // unrestricted config in init() and restores the original one in cleanup().
    const QGeoCoordinate goal = vehicle()->coordinate().atDistanceAndAzimuth(150, 214);
    QJsonArray polygon;
    for (const double azimuth : {0.0, 90.0, 180.0, 270.0}) {
        const QGeoCoordinate corner = goal.atDistanceAndAzimuth(30, azimuth);
        polygon.append(QJsonArray{corner.latitude(), corner.longitude()});
    }
    QFile config(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/SwarmConfig/fences.json");
    QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray bytes =
        QJsonDocument(QJsonObject{{"exclude", QJsonArray{QJsonObject{{"polygon", polygon}}}}}).toJson();
    QCOMPARE(config.write(bytes), bytes.size());
    config.close();

    mockLink()->clearReceivedMavCommandCounts();
    QSignalSpy feedback(_manager.get(), &SwarmManager::swarmCommandFeedback);
    QVERIFY(!_manager->gotoLocation(followers.first(), goal));
    QVERIFY(!feedback.isEmpty());
    QVERIFY2(feedback.last().first().toString().contains(QStringLiteral("越出电子围栏")),
             qPrintable(feedback.last().first().toString()));
    QVERIFY(_manager->formation()->transitStatus().value(QStringLiteral("kind")).toString() != QStringLiteral("goto"));
    // MockLink handles commands in order: once this probe has arrived, a reposition sent by the refused goto
    // would have arrived too.
    vehicle()->sendMavCommand(vehicle()->defaultComponentId(), MockLink::MAV_CMD_MOCKLINK_ALWAYS_RESULT_ACCEPTED,
                              false);
    QTRY_COMPARE_WITH_TIMEOUT(mockLink()->receivedMavCommandCount(MockLink::MAV_CMD_MOCKLINK_ALWAYS_RESULT_ACCEPTED), 1,
                              TestTimeout::longMs());
    QCOMPARE(mockLink()->receivedMavCommandCount(MAV_CMD_DO_REPOSITION), 0);
}

void SwarmFormationTest::formationGotoOnUnguidedFollowerIsNative()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(1);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    mockLink()->setRepositionSupported(true);
    for (const QPointer<MockLink>& link : std::as_const(_extraLinks))
        link->setRepositionSupported(true);
    Vehicle* follower = followers.first();
    MockLink* followerLink = linkForVehicle(_extraLinks, follower->id());
    QVERIFY(followerLink);
    QVERIFY(_manager->startFormation(vehicle()->id()));
    QVERIFY(_manager->formation()->guides(follower->id()));

    // A pending formation command would make the native goto a duplicate the command queue refuses to send.
    QTRY_VERIFY_WITH_TIMEOUT(!follower->isMavCommandPending(follower->defaultComponentId(), MAV_CMD_DO_REPOSITION),
                             TestTimeout::longMs());
    followerLink->clearReceivedMavlinkMessageCounts();
    // A follower in failsafe is no longer guided by the formation, so a goto on it is its own native goto
    // (for example to steer it during a return). MockLink's own heartbeat clears the failsafe about a second
    // later, so the goto follows right after the failsafe lands.
    reportFailsafe(followerLink, follower);
    QTRY_COMPARE_WITH_TIMEOUT(follower->systemStatus(), uint8_t(MAV_STATE_CRITICAL), TestTimeout::longMs());
    QVERIFY(!_manager->formation()->guides(follower->id()));
    const QGeoCoordinate goal = follower->coordinate().atDistanceAndAzimuth(80, 180);
    QVERIFY(_manager->gotoLocation(follower, goal));
    QVERIFY(_manager->formation()->transitStatus().value(QStringLiteral("kind")).toString() != QStringLiteral("goto"));
    QTRY_VERIFY_WITH_TIMEOUT(
        nativeGotoTarget(followerLink).isValid() && nativeGotoTarget(followerLink).distanceTo(goal) < 1.0,
        TestTimeout::longMs());
}

void SwarmFormationTest::px4RepositionCarriesGroundSpeed()
{
    const QGeoCoordinate target(47.3975, 8.5460, 520.5);
    QSignalSpy results(vehicle(), &Vehicle::mavCommandResult);
    // Scans backwards instead of indexing results.last(): an unrelated mavCommandResult racing in
    // (e.g. a late ack from a previous command) must not be mistaken for this command's result.
    const auto lastRepositionResult = [&results]() -> int {
        for (int i = results.count() - 1; i >= 0; --i) {
            if (results.at(i).at(2).toInt() == MAV_CMD_DO_REPOSITION)
                return results.at(i).at(3).toInt();
        }
        return -1;
    };

    QVERIFY(vehicle()->firmwarePlugin()->guidedModeGotoLocationAtSpeed(vehicle(), target, 4.5));
    QTRY_VERIFY_WITH_TIMEOUT(lastFormationReposition(mockLink()).valid(), TestTimeout::longMs());
    const Reposition command = lastFormationReposition(mockLink());
    QCOMPARE(command.speed, 4.5);
    QVERIFY(command.target.distanceTo(target) < 0.05);
    QCOMPARE(command.target.altitude(), 520.5);

    // lastFormationReposition() only decodes target/speed; param2 (the reposition flags) and
    // param4 (yaw, left NaN so the vehicle's yaw mode decides heading) need the raw message.
    mavlink_message_t rawMessage{};
    QVERIFY(mockLink()->lastReceivedMavlinkMessage(MAVLINK_MSG_ID_COMMAND_INT, rawMessage));
    mavlink_command_int_t rawCommand{};
    mavlink_msg_command_int_decode(&rawMessage, &rawCommand);
    QCOMPARE(rawCommand.param2, static_cast<float>(MAV_DO_REPOSITION_FLAGS_CHANGE_MODE));
    QVERIFY(std::isnan(rawCommand.param4));

    // MockLink rejects COMMAND_INT repositions unless they are enabled, then accepts them.
    QTRY_VERIFY_WITH_TIMEOUT(lastRepositionResult() >= 0, TestTimeout::longMs());
    QCOMPARE(lastRepositionResult(), int(MAV_RESULT_UNSUPPORTED));
    mockLink()->setRepositionSupported(true);
    results.clear();
    QVERIFY(vehicle()->firmwarePlugin()->guidedModeGotoLocationAtSpeed(vehicle(), target, 0));
    QTRY_VERIFY_WITH_TIMEOUT(lastRepositionResult() >= 0, TestTimeout::longMs());
    QCOMPARE(lastRepositionResult(), int(MAV_RESULT_ACCEPTED));
    QCOMPARE(lastFormationReposition(mockLink()).speed, -1.0);
    QCOMPARE(mockLink()->vehicleAltitudeAMSL(), 520.5);
}

void SwarmFormationTest::formationStartConvergesInSync()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(2);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    mockLink()->setRepositionSupported(true);
    for (const QPointer<MockLink>& link : std::as_const(_extraLinks))
        link->setRepositionSupported(true);
    mockLink()->clearReceivedMavCommandCounts();
    QHash<int, QGeoCoordinate> starts;
    QSet<int> members{vehicle()->id()};
    for (Vehicle* follower : std::as_const(followers)) {
        starts.insert(follower->id(), follower->coordinate());
        members.insert(follower->id());
    }
    FormationController formation;
    formation.setMembers(members);
    // MockLink places later vehicles north-east of the leader. Facing south-west puts the wedge slots on
    // that side, so the synchronized straight-line paths stay clear of the hovering leader.
    vehicle()->heading()->setRawValue(214);
    setLeaderGroundSpeed(vehicle(), 0.0);
    QVERIFY(formation.start(vehicle()->id()));
    QVERIFY(formation.transitActive());
    QCOMPARE(formation.transitStatus().value(QStringLiteral("kind")).toString(), QStringLiteral("reshape"));
    // An in-place reshape never commands the leader: it keeps whatever it is doing.
    QVERIFY(!vehicle()->isMavCommandPending(vehicle()->defaultComponentId(), MAV_CMD_DO_REPOSITION));
    QTRY_VERIFY_WITH_TIMEOUT(
        std::all_of(_extraLinks.cbegin(), _extraLinks.cend(),
                    [](const QPointer<MockLink>& link) { return lastFormationReposition(link).valid(); }),
        TestTimeout::longMs());
    QList<double> arrivalSeconds;
    for (const QPointer<MockLink>& link : std::as_const(_extraLinks)) {
        const Reposition command = lastFormationReposition(link);
        QVERIFY(command.speed > 0);
        arrivalSeconds.append(starts.value(link->vehicleId()).distanceTo(command.target) / command.speed);
    }
    QVERIFY2(qAbs(arrivalSeconds.first() - arrivalSeconds.last()) < 0.5,
             qPrintable(QStringLiteral("arrivals %1 s / %2 s").arg(arrivalSeconds.first()).arg(arrivalSeconds.last())));
    // Nor in any later cycle of the leg.
    QCOMPARE(mockLink()->receivedMavCommandCount(MAV_CMD_DO_REPOSITION), 0);
    formation.stop();
    QVERIFY(!formation.transitActive());
}

void SwarmFormationTest::formationReshapeWaitsForLeaderToStop()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(2);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    mockLink()->setRepositionSupported(true);
    for (const QPointer<MockLink>& link : std::as_const(_extraLinks))
        link->setRepositionSupported(true);
    FormationController formation;
    formation.setMembers({vehicle()->id(), followers.first()->id(), followers.last()->id()});
    // Facing south-west puts the wedge slots on the followers' side (see formationStartConvergesInSync). The
    // cycles below run back to back, so no telemetry overrides the heading or the ground speed in between.
    vehicle()->heading()->setRawValue(214);
    // While the leader is flown elsewhere the in-place reshape cannot start; station keeping guides the followers.
    setLeaderGroundSpeed(vehicle(), 2.0);
    QVERIFY(formation.start(vehicle()->id()));
    QVERIFY(!formation.transitActive());
    QVERIFY(QMetaObject::invokeMethod(&formation, "_updateFormation", Qt::DirectConnection));
    QVERIFY(!formation.transitActive());
    // Once the leader stops, the next cycle starts the reshape that was put off.
    setLeaderGroundSpeed(vehicle(), 0.0);
    QVERIFY(QMetaObject::invokeMethod(&formation, "_updateFormation", Qt::DirectConnection));
    QVERIFY(formation.transitActive());
    QCOMPARE(formation.transitStatus().value(QStringLiteral("kind")).toString(), QStringLiteral("reshape"));
    formation.stop();
}

void SwarmFormationTest::formationRoutePreviewSkipsFollowersWithoutSlot()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(2);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    const QSet<int> members{vehicle()->id(), followers.first()->id(), followers.last()->id()};
    FormationController formation;
    formation.setMembers(members);
    vehicle()->heading()->setRawValue(214);
    // A moving leader skips the synchronized reshape leg at start.
    setLeaderGroundSpeed(vehicle(), 2.0);
    QVERIFY(formation.start(vehicle()->id()));
    // Refreshing the members drops every slot until the next guidance cycle rebuilds them. The preview still
    // plans the route: the leader flies it, the slot-less (airborne) followers only have to be kept clear of.
    formation.setMembers(members);
    QGeoCoordinate goal = vehicle()->coordinate().atDistanceAndAzimuth(100, 214);
    goal.setAltitude(vehicle()->altitudeAMSL()->rawValue().toDouble());
    const FormationLegPlanner::Plan preview = formation.planRoute({goal});
    QVERIFY2(preview.ok(), qPrintable(preview.error));
    for (const FormationLegPlanner::Leg& leg : preview.legs) {
        QCOMPARE(leg.members.size(), 1);
        QCOMPARE(leg.members.first().id, vehicle()->id());
    }
    formation.stop();
}

void SwarmFormationTest::formationLegRequiresLeaderLease()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(1);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    FormationController formation;
    formation.setMembers({vehicle()->id(), followers.first()->id()});
    bool leaderAllowed = false;
    const int leaderId = vehicle()->id();
    formation.setVehicleAutomationAllowedCallback([&](int id) { return id != leaderId || leaderAllowed; });
    setLeaderGroundSpeed(vehicle(), 2.0);
    QVERIFY(formation.start(leaderId));
    const QGeoCoordinate goal = vehicle()->coordinate().atDistanceAndAzimuth(100, 0);
    QString reason;
    QVERIFY(!formation.gotoFormation(goal, &reason));
    QVERIFY2(reason.contains(QStringLiteral("自动控制许可")), qPrintable(reason));
    QVERIFY(!formation.transitActive());
    leaderAllowed = true;
    QVERIFY2(formation.gotoFormation(goal, &reason), qPrintable(reason));
    QVERIFY(formation.transitActive());
    // Stopping the formation ends the active leg and reports it as not completed.
    QSignalSpy finished(&formation, &FormationController::transitFinished);
    formation.stop();
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.first().at(0).toBool(), false);
    QCOMPARE(finished.first().at(2).toString(), QStringLiteral("goto"));
    QVERIFY(!formation.transitActive());

    // A second, otherwise identical outcome is told apart by its serial.
    QVariantMap first = formation.transitStatus().value(QStringLiteral("last_result")).toMap();
    QVERIFY(formation.start(leaderId));
    QVERIFY2(formation.gotoFormation(goal, &reason), qPrintable(reason));
    formation.stop();
    QVariantMap second = formation.transitStatus().value(QStringLiteral("last_result")).toMap();
    QCOMPARE(second.take(QStringLiteral("serial")).toInt(), first.take(QStringLiteral("serial")).toInt() + 1);
    QVERIFY(second == first);
}

void SwarmFormationTest::formationSkipsMemberInFailsafe()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(2);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    Vehicle* failing = followers.first();
    Vehicle* healthy = followers.last();
    MockLink* failingLink = linkForVehicle(_extraLinks, failing->id());
    QVERIFY(failingLink);
    FormationController formation;
    formation.setMembers({vehicle()->id(), failing->id(), healthy->id()});
    QSet<int> commanded;
    formation.setTargetAllowedCallback([&](int id, const QGeoCoordinate&) {
        commanded.insert(id);
        return true;
    });
    // A moving leader skips the synchronized reshape leg; this test covers station keeping.
    setLeaderGroundSpeed(vehicle(), 2.0);
    QVERIFY(formation.start(vehicle()->id()));
    QVERIFY(!formation.transitActive());
    QCOMPARE(commanded, (QSet<int>{failing->id(), healthy->id()}));

    // Both followers must be free to take a command in the cycle under test, so that only the failsafe can
    // explain a skipped follower (a follower awaiting an ACK is skipped too).
    QTRY_VERIFY_WITH_TIMEOUT(!failing->isMavCommandPending(failing->defaultComponentId(), MAV_CMD_DO_REPOSITION) &&
                                 !healthy->isMavCommandPending(healthy->defaultComponentId(), MAV_CMD_DO_REPOSITION),
                             TestTimeout::longMs());
    reportFailsafe(failingLink, failing);
    QTRY_COMPARE_WITH_TIMEOUT(failing->systemStatus(), uint8_t(MAV_STATE_CRITICAL), TestTimeout::longMs());
    commanded.clear();
    QVERIFY(QMetaObject::invokeMethod(&formation, "_updateFormation", Qt::DirectConnection));
    QVERIFY2(!commanded.contains(failing->id()), "a follower in failsafe must not receive formation commands");
    QVERIFY2(commanded.contains(healthy->id()), "the other follower keeps station");
    formation.stop();
}

void SwarmFormationTest::formationLegAbortsWhenRepositionRejected()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(1);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    // Repositioning stays disabled: MockLink answers DO_REPOSITION with MAV_RESULT_UNSUPPORTED.
    FormationController formation;
    formation.setMembers({vehicle()->id(), followers.first()->id()});
    // A moving leader skips the synchronized reshape leg.
    setLeaderGroundSpeed(vehicle(), 2.0);
    QVERIFY(formation.start(vehicle()->id()));
    QSignalSpy finished(&formation, &FormationController::transitFinished);
    QString reason;
    QVERIFY2(formation.gotoFormation(vehicle()->coordinate().atDistanceAndAzimuth(100, 0), &reason),
             qPrintable(reason));
    QVERIFY(formation.transitActive());
    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(finished.cbegin(), finished.cend(),
                    [](const QList<QVariant>& arguments) {
                        return !arguments.at(0).toBool() &&
                               arguments.at(1).toString().contains(QStringLiteral("拒绝航段指令")) &&
                               arguments.at(2).toString() == QStringLiteral("goto");
                    }),
        TestTimeout::longMs());
    QVERIFY(!formation.transitActive());
    formation.stop();
}

void SwarmFormationTest::formationDropsFollowerEnteringFailsafeMidTransit()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(2);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    mockLink()->setRepositionSupported(true);
    for (const QPointer<MockLink>& link : std::as_const(_extraLinks))
        link->setRepositionSupported(true);
    Vehicle* failing = followers.first();
    Vehicle* healthy = followers.last();
    MockLink* failingLink = linkForVehicle(_extraLinks, failing->id());
    QVERIFY(failingLink);
    FormationController formation;
    formation.setMembers({vehicle()->id(), failing->id(), healthy->id()});
    // MockLink places the followers north-east of the leader; flying south-west keeps them behind it on a
    // single leg. A moving leader skips the synchronized reshape leg at start.
    vehicle()->heading()->setRawValue(214);
    setLeaderGroundSpeed(vehicle(), 2.0);
    QVERIFY(formation.start(vehicle()->id()));
    QVERIFY(!formation.transitActive());
    QString reason;
    QVERIFY2(formation.gotoFormation(vehicle()->coordinate().atDistanceAndAzimuth(100, 214), &reason),
             qPrintable(reason));
    QCOMPARE(formation.transitLegCount(), 1);
    QCOMPARE(transitMemberIds(formation), (QSet<int>{vehicle()->id(), failing->id(), healthy->id()}));

    reportFailsafe(failingLink, failing);
    QTRY_COMPARE_WITH_TIMEOUT(failing->systemStatus(), uint8_t(MAV_STATE_CRITICAL), TestTimeout::longMs());
    QVERIFY(QMetaObject::invokeMethod(&formation, "_updateFormation", Qt::DirectConnection));
    // The follower in failsafe leaves the leg; the remaining pairs were already checked, so the leg continues.
    QVERIFY(formation.transitActive());
    QCOMPARE(transitMemberIds(formation), (QSet<int>{vehicle()->id(), healthy->id()}));
    formation.stop();
}

void SwarmFormationTest::formationMemberLeavesLegOnModeChange()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(2);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    QHash<int, MockLink*> links{{vehicle()->id(), mockLink()}};
    for (const QPointer<MockLink>& link : std::as_const(_extraLinks))
        links.insert(link->vehicleId(), link);
    for (MockLink* link : std::as_const(links))
        link->setRepositionSupported(true);
    Vehicle* leaving = followers.first();
    Vehicle* staying = followers.last();
    FormationController formation;
    formation.setMembers({vehicle()->id(), leaving->id(), staying->id()});
    // MockLink places the followers north-east of the leader; flying south-west keeps them behind it on a
    // single leg. A moving leader skips the synchronized reshape leg at start.
    vehicle()->heading()->setRawValue(214);
    setLeaderGroundSpeed(vehicle(), 2.0);
    QVERIFY(formation.start(vehicle()->id()));
    QSignalSpy finished(&formation, &FormationController::transitFinished);
    QString reason;
    QVERIFY2(formation.gotoFormation(vehicle()->coordinate().atDistanceAndAzimuth(100, 214), &reason),
             qPrintable(reason));
    QCOMPARE(formation.transitLegCount(), 1);
    QCOMPARE(transitMemberIds(formation), (QSet<int>{vehicle()->id(), leaving->id(), staying->id()}));

    // Every member accepted its leg command (formation legs carry a ground speed) and, like PX4, MockLink then
    // reports the goto mode.
    const QString gotoMode = vehicle()->gotoFlightMode();
    auto* mvm = MultiVehicleManager::instance();
    QTRY_VERIFY_WITH_TIMEOUT(std::all_of(links.cbegin(), links.cend(),
                                         [mvm, &gotoMode](MockLink* link) {
                                             Vehicle* member = mvm->getVehicleById(link->vehicleId());
                                             return member && lastFormationReposition(link).speed > 0 &&
                                                    !member->isMavCommandPending(member->defaultComponentId(),
                                                                                 MAV_CMD_DO_REPOSITION) &&
                                                    member->flightMode() == gotoMode;
                                         }),
                             TestTimeout::longMs());
    // A guidance cycle observes every member in the goto mode; nobody has left yet.
    QVERIFY(QMetaObject::invokeMethod(&formation, "_updateFormation", Qt::DirectConnection));
    QCOMPARE(transitMemberIds(formation), (QSet<int>{vehicle()->id(), leaving->id(), staying->id()}));

    // A follower switched to another mode (here a native single-vehicle landing) has been taken over: it leaves
    // the leg, so no speed correction pulls it back into the goto mode, and the others continue.
    leaving->setFlightMode(leaving->landFlightMode());
    QTRY_VERIFY_WITH_TIMEOUT(!transitMemberIds(formation).contains(leaving->id()), TestTimeout::longMs());
    QVERIFY(formation.transitActive());
    QCOMPARE(transitMemberIds(formation), (QSet<int>{vehicle()->id(), staying->id()}));
    QVERIFY(finished.isEmpty());

    // The leader switching away ends the leg.
    vehicle()->setFlightMode(vehicle()->rtlFlightMode());
    QTRY_VERIFY_WITH_TIMEOUT(!formation.transitActive(), TestTimeout::longMs());
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.first().at(0).toBool(), false);
    const QString finishReason = finished.first().at(1).toString();
    QVERIFY2(finishReason.contains(QStringLiteral("已切换")), qPrintable(finishReason));
    formation.stop();
}

void SwarmFormationTest::formationRejectsLegWhenLeaderInFailsafe()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(1);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    FormationController formation;
    formation.setMembers({vehicle()->id(), followers.first()->id()});
    // A moving leader skips the synchronized reshape leg.
    setLeaderGroundSpeed(vehicle(), 2.0);
    QVERIFY(formation.start(vehicle()->id()));
    const QGeoCoordinate goal = vehicle()->coordinate().atDistanceAndAzimuth(100, 0);

    reportFailsafe(mockLink(), vehicle());
    QTRY_COMPARE_WITH_TIMEOUT(vehicle()->systemStatus(), uint8_t(MAV_STATE_CRITICAL), TestTimeout::longMs());
    QString reason;
    QVERIFY(!formation.gotoFormation(goal, &reason));
    QVERIFY2(reason.contains(QStringLiteral("失效保护")), qPrintable(reason));
    QVERIFY(!formation.transitActive());
    // The route preview applies the same preconditions as starting the route.
    const QString previewError = formation.planRoute({goal}).error;
    QVERIFY2(previewError.contains(QStringLiteral("失效保护")), qPrintable(previewError));
    // The next guidance cycle blocks the formation instead of commanding anyone.
    QVERIFY(QMetaObject::invokeMethod(&formation, "_updateFormation", Qt::DirectConnection));
    QVERIFY(formation.blocked());
    formation.stop();
}

void SwarmFormationTest::formationStrandedLeaderIsNotGuided()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(2);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    const int leaderId = vehicle()->id();
    FormationController formation;
    formation.setMembers({leaderId, followers.first()->id(), followers.last()->id()});
    // Neither follower is a valid successor, so a leader handover below has nobody to hand off to.
    formation.setVehicleAutomationAllowedCallback([leaderId](int id) { return id == leaderId; });
    QVERIFY(formation.start(leaderId));
    QVERIFY(formation.guides(leaderId));
    formation.removeMembers({leaderId});
    // Handover failed: _leaderVehicleId still names the departed leader, but that id is no longer a member, so
    // the (now stranded) leader must not be treated as guided -- goto on it should take the native path instead.
    QVERIFY(!formation.guides(leaderId));
    formation.stop();
}

void SwarmFormationTest::formationGotoOnStrandedLeaderIsNative()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(2);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    mockLink()->setRepositionSupported(true);
    for (const QPointer<MockLink>& link : std::as_const(_extraLinks))
        link->setRepositionSupported(true);
    Vehicle* leader = vehicle();
    const int leaderId = leader->id();
    QVERIFY(_manager->startFormation(leaderId));
    QVERIFY(_manager->formation()->guides(leaderId));

    // Neither follower can take over: both are in failsafe when the leader is cancelled below.
    for (Vehicle* follower : std::as_const(followers)) {
        MockLink* followerLink = linkForVehicle(_extraLinks, follower->id());
        QVERIFY(followerLink);
        reportFailsafe(followerLink, follower);
        QTRY_COMPARE_WITH_TIMEOUT(follower->systemStatus(), uint8_t(MAV_STATE_CRITICAL), TestTimeout::longMs());
    }
    mockLink()->clearReceivedMavlinkMessageCounts();
    _manager->cancelTargets({leaderId}, QStringLiteral("test"));
    QVERIFY(!_manager->formation()->guides(leaderId));

    const QGeoCoordinate goal = leader->coordinate().atDistanceAndAzimuth(80, 180);
    QVERIFY(_manager->gotoLocation(leader, goal));
    QTRY_VERIFY_WITH_TIMEOUT(
        nativeGotoTarget(mockLink()).isValid() && nativeGotoTarget(mockLink()).distanceTo(goal) < 1.0,
        TestTimeout::longMs());
}

UT_REGISTER_TEST(SwarmFormationTest, TestLabel::Integration, TestLabel::Vehicle)
