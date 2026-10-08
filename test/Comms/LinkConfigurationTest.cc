#include "LinkConfigurationTest.h"

#include "BluetoothConfiguration.h"
#include "LinkConfiguration.h"
#include "LogReplayLink.h"
#ifndef QGC_NO_SERIAL_LINK
#include "SerialLink.h"
#endif
#ifdef QT_DEBUG
#include "MockConfiguration.h"
#endif
#include <QtCore/QMetaProperty>
#include <QtCore/QSettings>
#include <QtNetwork/QNetworkDatagram>
#include <QtNetwork/QUdpSocket>
#include <QtTest/QSignalSpy>
#include <memory>

#include "Fixtures/RAIIFixtures.h"
#include "QGCMAVLink.h"
#include "TCPLink.h"
#include "UDPLink.h"

void LinkConfigurationTest::_testUdpRoutesVehicleCommands_data()
{
    QTest::addColumn<bool>("mavlink1");
    QTest::addColumn<bool>("commandInt");
    QTest::addColumn<int>("targetSystem");
    QTest::addColumn<bool>("signedFrames");
    QTest::newRow("v1-command-long") << true << false << 1 << false;
    QTest::newRow("v2-command-long") << false << false << 1 << false;
    QTest::newRow("v2-command-int") << false << true << 1 << false;
    QTest::newRow("routed-system") << false << true << 3 << false;
    QTest::newRow("broadcast") << false << true << 0 << false;
    QTest::newRow("unknown-system") << false << true << 99 << false;
    QTest::newRow("signed-command") << false << true << 1 << true;
    QTest::newRow("combined-signed-commands") << false << true << -1 << true;
}

void LinkConfigurationTest::_testUdpRoutesVehicleCommands()
{
    QFETCH(bool, mavlink1);
    QFETCH(bool, commandInt);
    QFETCH(int, targetSystem);
    QFETCH(bool, signedFrames);
    UDPConfiguration config(QStringLiteral("Routing test"));
    UDPWorker worker(&config);
    worker.setupSocket();
    worker.connectLink();
    QVERIFY(worker.isConnected());
    auto* socket = worker.findChild<QUdpSocket*>();
    QVERIFY(socket);
    const quint16 port = socket->localPort();
    QUdpSocket first;
    QUdpSocket second;
    QVERIFY(first.bind(QHostAddress::LocalHost, 0));
    QVERIFY(second.bind(QHostAddress::LocalHost, 0));

    const auto bytes = [](const mavlink_message_t& message) {
        QByteArray data(MAVLINK_MAX_PACKET_LEN, '\0');
        data.resize(mavlink_msg_to_send_buffer(reinterpret_cast<uint8_t*>(data.data()), &message));
        return data;
    };
    mavlink_status_t status{};
    status.flags = mavlink1 ? MAVLINK_STATUS_FLAG_OUT_MAVLINK1 : 0;
    mavlink_signing_t signing{};
    signing.flags = MAVLINK_SIGNING_FLAG_SIGN_OUTGOING;
    signing.timestamp = 12345;
    status.signing = signedFrames ? &signing : nullptr;
    const auto heartbeat = [&](uint8_t systemId) {
        mavlink_message_t message{};
        mavlink_heartbeat_t payload{};
        payload.autopilot = MAV_AUTOPILOT_PX4;
        mavlink_msg_heartbeat_encode_status(systemId, MAV_COMP_ID_AUTOPILOT1, &status, &message, &payload);
        return bytes(message);
    };

    QSignalSpy received(&worker, &UDPWorker::dataReceived);
    const QByteArray firstHeartbeat = heartbeat(1) + heartbeat(3);
    QCOMPARE(first.writeDatagram(firstHeartbeat, QHostAddress::LocalHost, port), firstHeartbeat.size());
    QTRY_COMPARE_WITH_TIMEOUT(received.count(), 1, 1000);
    const QByteArray secondHeartbeat = heartbeat(2);
    QCOMPARE(second.writeDatagram(secondHeartbeat, QHostAddress::LocalHost, port), secondHeartbeat.size());
    QTRY_COMPARE_WITH_TIMEOUT(received.count(), 2, 1000);

    mavlink_message_t message{};
    if (commandInt) {
        mavlink_command_int_t command{};
        command.target_system = targetSystem == -1 ? 1 : targetSystem;
        command.target_component = MAV_COMP_ID_AUTOPILOT1;
        command.command = MAV_CMD_DO_REPOSITION;
        command.param2 = MAV_DO_REPOSITION_FLAGS_CHANGE_MODE;
        mavlink_msg_command_int_encode_status(255, MAV_COMP_ID_MISSIONPLANNER, &status, &message, &command);
    } else {
        mavlink_command_long_t command{};
        command.target_system = targetSystem;
        command.target_component = MAV_COMP_ID_AUTOPILOT1;
        command.command = MAV_CMD_DO_REPOSITION;
        command.param2 = MAV_DO_REPOSITION_FLAGS_CHANGE_MODE;
        mavlink_msg_command_long_encode_status(255, MAV_COMP_ID_MISSIONPLANNER, &status, &message, &command);
    }
    const QByteArray command = bytes(message);
    QByteArray otherCommand;
    if (targetSystem == -1) {
        mavlink_command_int_t other{};
        other.target_system = 2;
        other.target_component = MAV_COMP_ID_AUTOPILOT1;
        other.command = MAV_CMD_DO_REPOSITION;
        other.param2 = MAV_DO_REPOSITION_FLAGS_CHANGE_MODE;
        mavlink_msg_command_int_encode_status(255, MAV_COMP_ID_MISSIONPLANNER, &status, &message, &other);
        otherCommand = bytes(message);
    }
    worker.writeData(command + otherCommand);
    // This broadcast is an ordering barrier: the other drone must receive it first,
    // without having received a reposition command addressed to its neighbour.
    const QByteArray broadcast = heartbeat(255);
    worker.writeData(broadcast);
    QTRY_VERIFY_WITH_TIMEOUT(first.hasPendingDatagrams() && second.hasPendingDatagrams(), 1000);
    QCOMPARE(first.receiveDatagram().data(), command);
    QTRY_VERIFY_WITH_TIMEOUT(first.hasPendingDatagrams(), 1000);
    QCOMPARE(first.receiveDatagram().data(), broadcast);
    if (targetSystem == 0 || targetSystem == 99) {
        QCOMPARE(second.receiveDatagram().data(), command);
        QTRY_VERIFY_WITH_TIMEOUT(second.hasPendingDatagrams(), 1000);
    } else if (targetSystem == -1) {
        QCOMPARE(second.receiveDatagram().data(), otherCommand);
        QTRY_VERIFY_WITH_TIMEOUT(second.hasPendingDatagrams(), 1000);
    }
    QCOMPARE(second.receiveDatagram().data(), broadcast);
}

// ============================================================================
// LinkConfiguration base tests (exercised via TCPConfiguration)
// ============================================================================

void LinkConfigurationTest::_testBaseSetNameEmitsSignal()
{
    TCPConfiguration config(QStringLiteral("OriginalName"));
    QSignalSpy spy(&config, &LinkConfiguration::nameChanged);

    config.setName(QStringLiteral("NewName"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(config.name(), QStringLiteral("NewName"));

    // Same name must not re-emit
    config.setName(QStringLiteral("NewName"));
    QCOMPARE(spy.count(), 1);
}

void LinkConfigurationTest::_testBaseSetDynamicEmitsSignal()
{
    TCPConfiguration config(QStringLiteral("DynTest"));
    QSignalSpy spy(&config, &LinkConfiguration::dynamicChanged);

    config.setDynamic(true);
    QCOMPARE(spy.count(), 1);
    QVERIFY(config.isDynamic());

    // Same value must not re-emit
    config.setDynamic(true);
    QCOMPARE(spy.count(), 1);

    config.setDynamic(false);
    QCOMPARE(spy.count(), 2);
    QVERIFY(!config.isDynamic());
}

void LinkConfigurationTest::_testBaseSetAutoConnectEmitsSignal()
{
    TCPConfiguration config(QStringLiteral("AutoConnTest"));
    QSignalSpy spy(&config, &LinkConfiguration::autoConnectChanged);

    config.setAutoConnect(true);
    QCOMPARE(spy.count(), 1);
    QVERIFY(config.isAutoConnect());

    config.setAutoConnect(true);
    QCOMPARE(spy.count(), 1);

    config.setAutoConnect(false);
    QCOMPARE(spy.count(), 2);
}

void LinkConfigurationTest::_testBaseSetHighLatencyEmitsSignal()
{
    TCPConfiguration config(QStringLiteral("HLTest"));
    QSignalSpy spy(&config, &LinkConfiguration::highLatencyChanged);

    config.setHighLatency(true);
    QCOMPARE(spy.count(), 1);
    QVERIFY(config.isHighLatency());

    config.setHighLatency(true);
    QCOMPARE(spy.count(), 1);

    config.setHighLatency(false);
    QCOMPARE(spy.count(), 2);
}

void LinkConfigurationTest::_testBaseDefaults()
{
    TCPConfiguration config(QStringLiteral("DefaultsTest"));

    QVERIFY(!config.isDynamic());
    QVERIFY(!config.isAutoConnect());
    QVERIFY(!config.isHighLatency());
    QVERIFY(!config.isForwarding());
    QVERIFY(config.link() == nullptr);
}

void LinkConfigurationTest::_testBaseSettingsRoot()
{
    QCOMPARE(LinkConfiguration::settingsRoot(), QStringLiteral("LinkConfigurations"));
}

// ============================================================================
// TCPConfiguration tests
// ============================================================================

void LinkConfigurationTest::_testTcpConstruction()
{
    TCPConfiguration config(QStringLiteral("TestTCP"));

    QCOMPARE(config.name(), QStringLiteral("TestTCP"));
    QCOMPARE(config.type(), LinkConfiguration::TypeTcp);
    QCOMPARE(config.port(), quint16(5760));
    QVERIFY(config.host().isEmpty());
}

void LinkConfigurationTest::_testTcpSetHostEmitsSignal()
{
    TCPConfiguration config(QStringLiteral("TCPHostSignal"));
    QSignalSpy spy(&config, &TCPConfiguration::hostChanged);

    config.setHost(QStringLiteral("192.168.1.1"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(config.host(), QStringLiteral("192.168.1.1"));

    // Same value must not re-emit
    config.setHost(QStringLiteral("192.168.1.1"));
    QCOMPARE(spy.count(), 1);

    // setHost trims whitespace
    config.setHost(QStringLiteral("  10.0.0.1  "));
    QCOMPARE(spy.count(), 2);
    QCOMPARE(config.host(), QStringLiteral("10.0.0.1"));
}

void LinkConfigurationTest::_testTcpSetPortEmitsSignal()
{
    TCPConfiguration config(QStringLiteral("TCPPortSignal"));
    QSignalSpy spy(&config, &TCPConfiguration::portChanged);

    config.setPort(14550);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(config.port(), quint16(14550));

    // Same value must not re-emit
    config.setPort(14550);
    QCOMPARE(spy.count(), 1);
}

void LinkConfigurationTest::_testTcpCopyConstruction()
{
    TCPConfiguration original(QStringLiteral("TCPCopyOrig"));
    original.setHost(QStringLiteral("192.168.0.10"));
    original.setPort(5700);
    original.setDynamic(true);
    original.setHighLatency(true);

    TCPConfiguration copy(&original);

    QCOMPARE(copy.name(), QStringLiteral("TCPCopyOrig"));
    QCOMPARE(copy.host(), QStringLiteral("192.168.0.10"));
    QCOMPARE(copy.port(), quint16(5700));
    QVERIFY(copy.isDynamic());
    QVERIFY(copy.isHighLatency());
}

void LinkConfigurationTest::_testTcpCopyFrom()
{
    TCPConfiguration source(QStringLiteral("TCPSource"));
    source.setHost(QStringLiteral("10.0.0.5"));
    source.setPort(14550);

    TCPConfiguration dest(QStringLiteral("TCPDest"));
    dest.copyFrom(&source);

    QCOMPARE(dest.name(), QStringLiteral("TCPSource"));
    QCOMPARE(dest.host(), QStringLiteral("10.0.0.5"));
    QCOMPARE(dest.port(), quint16(14550));
}

void LinkConfigurationTest::_testTcpSettingsRoundtrip()
{
    // Isolate the test from the real application QSettings store: use an
    // IniFormat file inside a TempDirFixture so a crash/QFAIL cannot leave
    // LinkConfigTest_* keys in the user's config.
    TestFixtures::TempDirFixture tmpDir;
    QVERIFY(tmpDir.isValid());
    const QString iniPath = tmpDir.path() + QStringLiteral("/settings.ini");

    const QString root = QStringLiteral("LinkConfigTest_TCP");
    QSettings settings(iniPath, QSettings::IniFormat);

    {
        TCPConfiguration config(QStringLiteral("TCPSave"));
        config.setHost(QStringLiteral("172.16.0.1"));
        config.setPort(5762);
        config.saveSettings(settings, root);
    }

    {
        TCPConfiguration config(QStringLiteral("TCPLoad"));
        config.loadSettings(settings, root);
        QCOMPARE(config.host(), QStringLiteral("172.16.0.1"));
        QCOMPARE(config.port(), quint16(5762));
    }
}

void LinkConfigurationTest::_testTcpHostnameRoundtrip()
{
    TestFixtures::TempDirFixture tmpDir;
    QVERIFY(tmpDir.isValid());
    const QString iniPath = tmpDir.path() + QStringLiteral("/settings.ini");

    const QString root = QStringLiteral("LinkConfigTest_TCPHost");
    QSettings settings(iniPath, QSettings::IniFormat);

    {
        TCPConfiguration config(QStringLiteral("TCPSave"));
        config.setHost(QStringLiteral("my-drone.local"));
        config.setPort(5760);
        config.saveSettings(settings, root);
    }

    {
        TCPConfiguration config(QStringLiteral("TCPLoad"));
        config.loadSettings(settings, root);
        QCOMPARE(config.host(), QStringLiteral("my-drone.local"));
        QCOMPARE(config.port(), quint16(5760));
    }
}

void LinkConfigurationTest::_testSuppressAutoReconnectNotPersisted()
{
    TestFixtures::TempDirFixture tmpDir;
    QVERIFY(tmpDir.isValid());
    const QString iniPath = tmpDir.path() + QStringLiteral("/settings.ini");

    const QString root = QStringLiteral("LinkConfigTest_Suppress");
    QSettings settings(iniPath, QSettings::IniFormat);

    TCPConfiguration config(QStringLiteral("SuppressTest"));
    QVERIFY(!config.suppressAutoReconnect());

    config.setSuppressAutoReconnect(true);
    QVERIFY(config.suppressAutoReconnect());
    config.saveSettings(settings, root);

    TCPConfiguration loaded(QStringLiteral("SuppressLoad"));
    loaded.loadSettings(settings, root);
    QVERIFY(!loaded.suppressAutoReconnect());
}

void LinkConfigurationTest::_testReconnectBackoff()
{
    TCPConfiguration config(QStringLiteral("BackoffTest"));
    QVERIFY(config.reconnectReady());

    config.noteReconnectAttempt();
    QVERIFY(!config.reconnectReady());

    config.resetReconnectBackoff();
    QVERIFY(config.reconnectReady());
}

// ============================================================================
// UDPConfiguration tests
// ============================================================================

void LinkConfigurationTest::_testUdpConstruction()
{
    UDPConfiguration config(QStringLiteral("TestUDP"));

    QCOMPARE(config.name(), QStringLiteral("TestUDP"));
    QCOMPARE(config.type(), LinkConfiguration::TypeUdp);
    QCOMPARE(config.localPort(), quint16(0));
    QVERIFY(config.hostList().isEmpty());
    QVERIFY(config.targetHosts().isEmpty());
}

void LinkConfigurationTest::_testUdpAddRemoveHost()
{
    UDPConfiguration config(QStringLiteral("UDPHosts"));
    QSignalSpy spy(&config, &UDPConfiguration::hostListChanged);

    // addHost(host, port) with a numeric IP resolves immediately without DNS
    config.addHost(QStringLiteral("127.0.0.1"), 14550);
    QCOMPARE(config.targetHosts().size(), 1);
    QCOMPARE(spy.count(), 1);

    // Adding the same host/port must be a no-op
    config.addHost(QStringLiteral("127.0.0.1"), 14550);
    QCOMPARE(config.targetHosts().size(), 1);
    QCOMPARE(spy.count(), 1);

    // addHost with "host:port" string format
    config.addHost(QStringLiteral("192.168.1.2:5760"));
    QCOMPARE(config.targetHosts().size(), 2);
    QCOMPARE(spy.count(), 2);

    // hostList reflects the two entries
    QCOMPARE(config.hostList().size(), 2);

    // removeHost(host, port)
    config.removeHost(QStringLiteral("127.0.0.1"), 14550);
    QCOMPARE(config.targetHosts().size(), 1);
    QCOMPARE(spy.count(), 3);

    // removeHost with "host:port" string format
    config.removeHost(QStringLiteral("192.168.1.2:5760"));
    QCOMPARE(config.targetHosts().size(), 0);
    QCOMPARE(spy.count(), 4);
    QVERIFY(config.hostList().isEmpty());
}

void LinkConfigurationTest::_testUdpSetLocalPortEmitsSignal()
{
    UDPConfiguration config(QStringLiteral("UDPPort"));
    QSignalSpy spy(&config, &UDPConfiguration::localPortChanged);

    config.setLocalPort(14550);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(config.localPort(), quint16(14550));

    // Same value must not re-emit
    config.setLocalPort(14550);
    QCOMPARE(spy.count(), 1);

    config.setLocalPort(5760);
    QCOMPARE(spy.count(), 2);
}

void LinkConfigurationTest::_testUdpCopyConstruction()
{
    UDPConfiguration original(QStringLiteral("UDPCopyOrig"));
    original.setLocalPort(14550);
    original.addHost(QStringLiteral("10.0.0.1"), 14550);
    original.addHost(QStringLiteral("10.0.0.2"), 5760);

    UDPConfiguration copy(&original);

    QCOMPARE(copy.name(), QStringLiteral("UDPCopyOrig"));
    QCOMPARE(copy.localPort(), quint16(14550));
    QCOMPARE(copy.targetHosts().size(), 2);
    QCOMPARE(copy.hostList().size(), 2);
}

void LinkConfigurationTest::_testUdpCopyFrom()
{
    UDPConfiguration source(QStringLiteral("UDPSource"));
    source.setLocalPort(5760);
    source.addHost(QStringLiteral("192.168.1.1"), 14550);

    UDPConfiguration dest(QStringLiteral("UDPDest"));
    dest.copyFrom(&source);

    QCOMPARE(dest.name(), QStringLiteral("UDPSource"));
    QCOMPARE(dest.localPort(), quint16(5760));
    QCOMPARE(dest.targetHosts().size(), 1);

    const auto targets = dest.targetHosts();
    const auto& target = targets.constFirst();
    QCOMPARE(target->address.toString(), QStringLiteral("192.168.1.1"));
    QCOMPARE(target->port, quint16(14550));
}

void LinkConfigurationTest::_testUdpSettingsRoundtrip()
{
    // See _testTcpSettingsRoundtrip for rationale on the IniFormat isolation.
    TestFixtures::TempDirFixture tmpDir;
    QVERIFY(tmpDir.isValid());
    const QString iniPath = tmpDir.path() + QStringLiteral("/settings.ini");

    const QString root = QStringLiteral("LinkConfigTest_UDP");
    QSettings settings(iniPath, QSettings::IniFormat);

    {
        UDPConfiguration config(QStringLiteral("UDPSave"));
        config.setLocalPort(14550);
        config.addHost(QStringLiteral("192.168.10.1"), 14550);
        config.addHost(QStringLiteral("192.168.10.2"), 5760);
        config.saveSettings(settings, root);
    }

    {
        UDPConfiguration config(QStringLiteral("UDPLoad"));
        // Pre-populate the port key so loadSettings doesn't fall back to
        // SettingsManager (which is not available in unit tests). saveSettings
        // writes "port" unconditionally, so the value is already present.
        config.loadSettings(settings, root);

        QCOMPARE(config.localPort(), quint16(14550));
        QCOMPARE(config.targetHosts().size(), 2);
    }
}

void LinkConfigurationTest::_testUdpHostnamePreservedWhenUnresolved()
{
    ignoreLogMessage("Comms.UDPLink", QtWarningMsg, QRegularExpression("Could not resolve host"));
    UDPConfiguration config(QStringLiteral("UDPUnresolved"));
    config.addHost(QStringLiteral("drone.invalid"), 14550);

    const auto targets = config.targetHosts();
    QCOMPARE(targets.size(), 1);
    const auto target = targets.constFirst();
    QCOMPARE(target->hostname, QStringLiteral("drone.invalid"));
    QVERIFY(target->address.isNull());
    QCOMPARE(config.hostList().constFirst(), QStringLiteral("drone.invalid:14550"));
}

void LinkConfigurationTest::_testUdpHostnameRoundtrip()
{
    ignoreLogMessage("Comms.UDPLink", QtWarningMsg, QRegularExpression("Could not resolve host"));
    TestFixtures::TempDirFixture tmpDir;
    QVERIFY(tmpDir.isValid());
    const QString iniPath = tmpDir.path() + QStringLiteral("/settings.ini");

    const QString root = QStringLiteral("LinkConfigTest_UDPHost");
    QSettings settings(iniPath, QSettings::IniFormat);

    {
        UDPConfiguration config(QStringLiteral("UDPSave"));
        config.setLocalPort(14550);
        config.addHost(QStringLiteral("drone.invalid"), 14550);
        config.saveSettings(settings, root);
    }

    {
        UDPConfiguration config(QStringLiteral("UDPLoad"));
        config.loadSettings(settings, root);
        QCOMPARE(config.targetHosts().size(), 1);
        QCOMPARE(config.targetHosts().constFirst()->hostname, QStringLiteral("drone.invalid"));
    }
}

void LinkConfigurationTest::_testUdpRemoveByHostname()
{
    ignoreLogMessage("Comms.UDPLink", QtWarningMsg, QRegularExpression("Could not resolve host"));
    UDPConfiguration config(QStringLiteral("UDPRemoveByName"));
    config.addHost(QStringLiteral("drone.invalid"), 14550);
    QCOMPARE(config.targetHosts().size(), 1);

    config.removeHost(QStringLiteral("drone.invalid:14550"));
    QCOMPARE(config.targetHosts().size(), 0);
    QVERIFY(config.hostList().isEmpty());
}

void LinkConfigurationTest::_testUdpResolveHostsUpdatesAddress()
{
    UDPConfiguration config(QStringLiteral("UDPResolve"));
    config.addHost(QStringLiteral("localhost"), 14550);

    auto targets = config.targetHosts();
    QCOMPARE(targets.size(), 1);
    QCOMPARE(targets.constFirst()->hostname, QStringLiteral("localhost"));

    config.resolveHosts();

    targets = config.targetHosts();
    const QHostAddress resolved = targets.constFirst()->address;
    QVERIFY(!resolved.isNull());
    QVERIFY(resolved.isLoopback());
}

UT_REGISTER_TEST(LinkConfigurationTest, TestLabel::Unit, TestLabel::Comms)

void LinkConfigurationTest::_testInvalidCopy_data()
{
    QTest::addColumn<int>("type");
    QTest::addColumn<QString>("category");
#ifndef QGC_NO_SERIAL_LINK
    QTest::newRow("serial") << int(LinkConfiguration::TypeSerial) << QStringLiteral("Comms.SerialLink");
#endif
    QTest::newRow("tcp") << int(LinkConfiguration::TypeTcp) << QStringLiteral("Comms.TCPLink");
    QTest::newRow("udp") << int(LinkConfiguration::TypeUdp) << QStringLiteral("Comms.UDPLink");
    QTest::newRow("bluetooth") << int(LinkConfiguration::TypeBluetooth)
                               << QStringLiteral("Comms.Bluetooth.BluetoothConfiguration");
    QTest::newRow("replay") << int(LinkConfiguration::TypeLogReplay) << QStringLiteral("Comms.LogReplayLink");
#ifdef QT_DEBUG
    QTest::newRow("mock") << int(LinkConfiguration::TypeMock) << QStringLiteral("Comms.MockLink.MockConfiguration");
#endif
}

void LinkConfigurationTest::_testInvalidCopy()
{
    QFETCH(int, type);
    QFETCH(QString, category);
    if (type == LinkConfiguration::TypeBluetooth) {
        ignoreLogMessage("default", QtWarningMsg,
                         QRegularExpression(QStringLiteral("^Cannot find a compatible running Bluez\\.")));
    }
    const std::unique_ptr<LinkConfiguration> config(
        LinkConfiguration::createSettings(type, QStringLiteral("preserved")));
    QVERIFY(config);
    config->setDynamic(true);
    config->setHighLatency(true);
    const auto snapshot = [](LinkConfiguration* value) {
        QVariantMap properties;
        const auto* meta = value->metaObject();
        for (int i = 0; i < meta->propertyCount(); ++i) {
            const auto property = meta->property(i);
            if (property.isReadable())
                properties.insert(QString::fromLatin1(property.name()), property.read(value));
        }
        return properties;
    };
    const auto before = snapshot(config.get());
    expectLogMessage(qPrintable(category), QtWarningMsg,
                     QRegularExpression(QStringLiteral("Invalid source configuration type")));
    config->copyFrom(nullptr);
    verifyExpectedLogMessage();
    QCOMPARE(snapshot(config.get()), before);

    const auto otherType = type == LinkConfiguration::TypeTcp ? LinkConfiguration::TypeUdp : LinkConfiguration::TypeTcp;
    const std::unique_ptr<LinkConfiguration> wrongType(
        LinkConfiguration::createSettings(otherType, QStringLiteral("wrong")));
    QVERIFY(wrongType);
    expectLogMessage(qPrintable(category), QtWarningMsg,
                     QRegularExpression(QStringLiteral("Invalid source configuration type")));
    config->copyFrom(wrongType.get());
    verifyExpectedLogMessage();
    QCOMPARE(snapshot(config.get()), before);

    expectLogMessage("Comms.LinkConfiguration", QtWarningMsg,
                     QRegularExpression(QStringLiteral("Cannot copy a null link configuration")));
    std::unique_ptr<LinkConfiguration> empty;
    switch (type) {
#ifndef QGC_NO_SERIAL_LINK
        case LinkConfiguration::TypeSerial:
            empty = std::make_unique<SerialConfiguration>(static_cast<const SerialConfiguration*>(nullptr));
            break;
#endif
        case LinkConfiguration::TypeTcp:
            empty = std::make_unique<TCPConfiguration>(static_cast<const TCPConfiguration*>(nullptr));
            break;
        case LinkConfiguration::TypeUdp:
            empty = std::make_unique<UDPConfiguration>(static_cast<const UDPConfiguration*>(nullptr));
            break;
        case LinkConfiguration::TypeBluetooth:
            empty = std::make_unique<BluetoothConfiguration>(static_cast<const BluetoothConfiguration*>(nullptr));
            break;
        case LinkConfiguration::TypeLogReplay:
            empty = std::make_unique<LogReplayConfiguration>(static_cast<const LogReplayConfiguration*>(nullptr));
            break;
#ifdef QT_DEBUG
        case LinkConfiguration::TypeMock:
            empty = std::make_unique<MockConfiguration>(static_cast<const MockConfiguration*>(nullptr));
            break;
#endif
        default:
            QFAIL("Unhandled link type");
    }
    verifyExpectedLogMessage();
    QVERIFY(empty);
    QVERIFY(empty->name().isEmpty());
    QVERIFY(!empty->link());
    QVERIFY(!empty->isDynamic());
    QVERIFY(!empty->isHighLatency());
}

void LinkConfigurationTest::_testNullDuplicate()
{
    expectLogMessage("Comms.LinkConfiguration", QtWarningMsg,
                     QRegularExpression(QStringLiteral("Cannot duplicate a null link configuration")));
    QVERIFY(!LinkConfiguration::duplicateSettings(nullptr));
    verifyExpectedLogMessage();
}

void LinkConfigurationTest::_testUdpSelfCopy()
{
    UDPConfiguration config(QStringLiteral("self"));
    config.addHost(QStringLiteral("127.0.0.1"), 14550);
    config.setLocalPort(14551);
    const auto hosts = config.hostList();
    QVERIFY(!hosts.isEmpty());
    config.copyFrom(&config);
    QCOMPARE(config.hostList(), hosts);
    QCOMPARE(config.localPort(), quint16(14551));
}
