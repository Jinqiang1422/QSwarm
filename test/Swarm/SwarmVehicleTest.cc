#include "SwarmVehicleTest.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopeGuard>
#include <QtCore/QStandardPaths>
#include <algorithm>

#include "Fact.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "SwarmClock.h"
#include "SwarmManager.h"
#include "Vehicle.h"
#include "VehicleFactGroup.h"

SwarmVehicleTest::~SwarmVehicleTest() = default;

void SwarmVehicleTest::init()
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

void SwarmVehicleTest::cleanup()
{
    // Guarantee the base teardown runs even if a QVERIFY/QTRY below fails.
    const auto base = qScopeGuard([this] { VehicleTest::cleanup(); });
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
}

void SwarmVehicleTest::addVehicles(int count)
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

void SwarmVehicleTest::takeOffAll(QList<Vehicle*>& followers)
{
    auto* mvm = MultiVehicleManager::instance();
    followers.clear();
    QList<Vehicle*> members;
    for (int i = 0; i < mvm->vehicles()->count(); ++i) {
        auto* member = qobject_cast<Vehicle*>(mvm->vehicles()->get(i));
        QVERIFY(member);
        member->sendMavCommand(member->defaultComponentId(), MAV_CMD_NAV_TAKEOFF, true, 0, 0, 0, 0, 0, 0, 10);
        members.append(member);
        if (member != vehicle())
            followers.append(member);
    }
    QTRY_VERIFY_WITH_TIMEOUT(
        std::all_of(members.cbegin(), members.cend(),
                    [](Vehicle* v) { return v->armed() && v->altitudeRelative()->rawValue().toDouble() >= 9; }),
        TestTimeout::longMs());
    std::sort(followers.begin(), followers.end(), [](Vehicle* a, Vehicle* b) { return a->id() < b->id(); });
}

SwarmVehicleTest::Reposition SwarmVehicleTest::lastFormationReposition(MockLink* link)
{
    mavlink_message_t message{};
    if (!link || !link->lastReceivedMavlinkMessage(MAVLINK_MSG_ID_COMMAND_INT, message))
        return {};
    mavlink_command_int_t command{};
    mavlink_msg_command_int_decode(&message, &command);
    if (command.command != MAV_CMD_DO_REPOSITION || command.frame != MAV_FRAME_GLOBAL)
        return {};
    return {QGeoCoordinate(command.x / 1e7, command.y / 1e7, command.z), command.param1};
}

void SwarmVehicleTest::setLeaderGroundSpeed(Vehicle* vehicle, double speed)
{
    if (!vehicle)
        return;
    auto* factGroup = qobject_cast<VehicleFactGroup*>(vehicle->vehicleFactGroup());
    if (!factGroup)
        return;
    factGroup->groundSpeed()->setRawValue(speed);
}
