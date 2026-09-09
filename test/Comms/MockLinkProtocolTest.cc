#include "MockLinkProtocolTest.h"

#include <QtCore/QScopeGuard>
#include <QtCore/QTemporaryFile>
#include <QtTest/QSignalSpy>
#include <limits>

#include "LinkManager.h"
#include "MockLinkFTP.h"

namespace {
mavlink_message_t decodeResponse(const QList<QVariant>& arguments)
{
    mavlink_message_t buffer{};
    mavlink_message_t message{};
    mavlink_status_t status{};
    mavlink_status_t decodedStatus{};
    for (const char byte : arguments.at(1).toByteArray()) {
        if (mavlink_frame_char_buffer(&buffer, &status, static_cast<uint8_t>(byte), &message, &decodedStatus) ==
            MAVLINK_FRAMING_OK) {
            return message;
        }
    }
    return {};
}
}  // namespace

void MockLinkProtocolTest::init()
{
    UnitTest::init();
    SharedLinkConfigurationPtr config = std::make_shared<MockConfiguration>(QStringLiteral("Protocol regression"));
    _link = std::make_unique<MockLink>(config);
    // These tests drive requests synchronously without a connected Vehicle or
    // background traffic.
    _link->disconnect();
    QVERIFY(_link->_allocateMavlinkChannel());
    // Use the same MAVLink 2 setup as a normal connection; the worker remains stopped.
    QVERIFY(_link->_connect());
    mavlink_get_channel_status(_link->mavlinkChannel())->flags &= ~MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
}

void MockLinkProtocolTest::cleanup()
{
    if (_link) {
        _link->_freeMavlinkChannel();
        _link.reset();
    }
    UnitTest::cleanup();
}

void MockLinkProtocolTest::_logReadPreservesBytes()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    const QByteArray contents =
        QByteArray("0123456789abcdefghijklmnopqrstuvwxyz").repeated(30).left(MockLink::_logDownloadFileSize);
    const uint32_t offset = contents.size() - 29;
    QCOMPARE(file.write(contents), contents.size());
    QVERIFY(file.flush());
    _link->_logDownloadFilename = file.fileName();

    QSignalSpy spy(_link.get(), &LinkInterface::bytesReceived);
    mavlink_message_t request{};
    // UINT32_MAX is the protocol's request-to-end value; adding the offset must
    // not wrap.
    mavlink_msg_log_request_data_pack_chan(255, 190, _link->mavlinkChannel(), &request, _link->vehicleId(),
                                           MAV_COMP_ID_AUTOPILOT1, 0, offset, std::numeric_limits<uint32_t>::max());
    _link->_handleLogRequestData(request);
    _link->_logDownloadWorker();
    QCOMPARE(spy.count(), 1);
    const mavlink_message_t response = decodeResponse(spy.takeFirst());
    QCOMPARE(response.msgid, uint32_t(MAVLINK_MSG_ID_LOG_DATA));
    mavlink_log_data_t data{};
    mavlink_msg_log_data_decode(&response, &data);
    QCOMPARE(data.ofs, offset);
    QCOMPARE(data.count, contents.size() - offset);
    QCOMPARE(QByteArray(reinterpret_cast<const char*>(data.data), data.count), contents.mid(offset));
    _link->_logDownloadWorker();
    QCOMPARE(spy.count(), 0);
}

void MockLinkProtocolTest::_logReadFailure()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    QCOMPARE(file.write("abc"), qint64(3));
    QVERIFY(file.flush());
    _link->_logDownloadFilename = file.fileName();
    QSignalSpy spy(_link.get(), &LinkInterface::bytesReceived);
    mavlink_message_t request{};
    mavlink_msg_log_request_data_pack_chan(255, 190, _link->mavlinkChannel(), &request, _link->vehicleId(),
                                           MAV_COMP_ID_AUTOPILOT1, 0, 0, 100);
    _link->_handleLogRequestData(request);
    expectLogMessage("Comms.MockLink.MockLink", QtWarningMsg, QRegularExpression("unable to read requested log data"));
    _link->_logDownloadWorker();
    verifyExpectedLogMessage();
    QCOMPARE(spy.count(), 0);
    _link->_logDownloadWorker();
    QCOMPARE(spy.count(), 0);
}

void MockLinkProtocolTest::_invalidParameterRequests()
{
    QSignalSpy spy(_link.get(), &LinkInterface::bytesReceived);
    mavlink_message_t request{};
    for (const int index : {-2, 32767}) {
        mavlink_msg_param_request_read_pack_chan(255, 190, _link->mavlinkChannel(), &request, _link->vehicleId(),
                                                 MAV_COMP_ID_AUTOPILOT1, "", index);
        _link->_handleParamRequestRead(request);
        QCOMPARE(spy.count(), 0);
    }
    for (const int component : {int(MAV_COMP_ID_AUTOPILOT1), 254}) {
        mavlink_msg_param_set_pack_chan(255, 190, _link->mavlinkChannel(), &request, _link->vehicleId(), component,
                                        "UNKNOWN_PARAM", 42, MAV_PARAM_TYPE_REAL32);
        _link->_handleParamSet(request);
        QCOMPARE(spy.count(), 0);
    }
    mavlink_msg_param_request_read_pack_chan(255, 190, _link->mavlinkChannel(), &request, _link->vehicleId() + 1,
                                             MAV_COMP_ID_AUTOPILOT1, "_HASH_CHECK", -1);
    _link->_handleParamRequestRead(request);
    QCOMPARE(spy.count(), 0);
    mavlink_msg_param_request_read_pack_chan(255, 190, _link->mavlinkChannel(), &request, _link->vehicleId(),
                                             MAV_COMP_ID_AUTOPILOT1, "", 0);
    _link->_handleParamRequestRead(request);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(decodeResponse(spy.takeFirst()).msgid, uint32_t(MAVLINK_MSG_ID_PARAM_VALUE));
}

void MockLinkProtocolTest::_ftpReadSession_data()
{
    QTest::addColumn<int>("opcode");
    QTest::newRow("read") << int(MavlinkFTP::kCmdReadFile);
    QTest::newRow("burst") << int(MavlinkFTP::kCmdBurstReadFile);
}

void MockLinkProtocolTest::_ftpReadSession()
{
    QFETCH(int, opcode);
    QSignalSpy spy(_link.get(), &LinkInterface::bytesReceived);
    MavlinkFTP::Request payload{};
    payload.hdr.opcode = opcode;
    payload.hdr.session = 1;
    payload.hdr.size = sizeof(payload.data);
    mavlink_message_t request{};
    mavlink_msg_file_transfer_protocol_pack_chan(255, 190, _link->mavlinkChannel(), &request, 0, _link->vehicleId(),
                                                 MAV_COMP_ID_AUTOPILOT1, reinterpret_cast<uint8_t*>(&payload));
    _link->mockLinkFTP()->mavlinkMessageReceived(request);
    QCOMPARE(spy.count(), 1);
    const mavlink_message_t response = decodeResponse(spy.takeFirst());
    QCOMPARE(response.msgid, uint32_t(MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL));
    mavlink_file_transfer_protocol_t ftp{};
    mavlink_msg_file_transfer_protocol_decode(&response, &ftp);
    MavlinkFTP::Request reply{};
    memcpy(&reply, ftp.payload, sizeof(ftp.payload));
    QCOMPARE(reply.hdr.opcode, uint8_t(MavlinkFTP::kRspNak));
    QCOMPARE(reply.data[0], uint8_t(MavlinkFTP::kErrInvalidSession));
}

void MockLinkProtocolTest::_ftpReadFailure_data()
{
    _ftpReadSession_data();
}

void MockLinkProtocolTest::_ftpReadFailure()
{
    QFETCH(int, opcode);
    QTemporaryFile file;
    QVERIFY(file.open());
    QCOMPARE(file.write("contents"), qint64(8));
    QVERIFY(file.flush());
    MockLinkFTP* ftpServer = _link->mockLinkFTP();
    QVERIFY(ftpServer);
    ftpServer->_currentFile.setFileName(file.fileName());
    // A file with a known size whose read operation fails must produce a NAK, never an ACK with garbage bytes.
    QVERIFY(ftpServer->_currentFile.open(QIODevice::WriteOnly | QIODevice::Append));
    QSignalSpy spy(_link.get(), &LinkInterface::bytesReceived);
    MavlinkFTP::Request payload{};
    payload.hdr.opcode = opcode;
    payload.hdr.session = 1;
    payload.hdr.size = sizeof(payload.data);
    mavlink_message_t request{};
    mavlink_msg_file_transfer_protocol_pack_chan(255, 190, _link->mavlinkChannel(), &request, 0, _link->vehicleId(),
                                                 MAV_COMP_ID_AUTOPILOT1, reinterpret_cast<uint8_t*>(&payload));
    expectLogMessage("default", QtWarningMsg, QRegularExpression("WriteOnly device"));
    ftpServer->mavlinkMessageReceived(request);
    verifyExpectedLogMessage();
    QCOMPARE(spy.count(), 1);
    const mavlink_message_t response = decodeResponse(spy.takeFirst());
    QCOMPARE(response.msgid, uint32_t(MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL));
    mavlink_file_transfer_protocol_t ftp{};
    mavlink_msg_file_transfer_protocol_decode(&response, &ftp);
    MavlinkFTP::Request reply{};
    memcpy(&reply, ftp.payload, sizeof(ftp.payload));
    QCOMPARE(reply.hdr.opcode, uint8_t(MavlinkFTP::kRspNak));
    QCOMPARE(reply.data[0], uint8_t(MavlinkFTP::kErrFail));
}

void MockLinkProtocolTest::_invalidMissionRequestPreservesItems()
{
    _link->loadSimpleMultirotorMission();
    QSignalSpy spy(_link.get(), &LinkInterface::bytesReceived);
    mavlink_message_t request{};
    mavlink_msg_mission_request_int_pack_chan(255, 190, _link->mavlinkChannel(), &request, _link->vehicleId(),
                                              MAV_COMP_ID_AUTOPILOT1, 99, MAV_MISSION_TYPE_MISSION);
    QVERIFY(_link->_missionItemHandler->handleMavlinkMessage(request));
    QCOMPARE(spy.count(), 1);
    mavlink_message_t response = decodeResponse(spy.takeFirst());
    QCOMPARE(response.msgid, uint32_t(MAVLINK_MSG_ID_MISSION_ACK));
    mavlink_mission_ack_t ack{};
    mavlink_msg_mission_ack_decode(&response, &ack);
    QCOMPARE(ack.type, uint8_t(MAV_MISSION_INVALID_SEQUENCE));
    mavlink_msg_mission_clear_all_pack_chan(255, 190, _link->mavlinkChannel(), &request, _link->vehicleId() + 1,
                                            MAV_COMP_ID_AUTOPILOT1, MAV_MISSION_TYPE_ALL);
    _link->_missionItemHandler->handleMavlinkMessage(request);
    QCOMPARE(spy.count(), 0);
    mavlink_msg_mission_request_list_pack_chan(255, 190, _link->mavlinkChannel(), &request, _link->vehicleId(),
                                               MAV_COMP_ID_AUTOPILOT1, MAV_MISSION_TYPE_MISSION);
    _link->_missionItemHandler->handleMavlinkMessage(request);
    QCOMPARE(spy.count(), 1);
    response = decodeResponse(spy.takeFirst());
    QCOMPARE(response.msgid, uint32_t(MAVLINK_MSG_ID_MISSION_COUNT));
    mavlink_mission_count_t count{};
    mavlink_msg_mission_count_decode(&response, &count);
    QCOMPARE(count.count, uint16_t(3));
}

void MockLinkProtocolTest::_missionResetCancelsUpload()
{
    QSignalSpy spy(_link.get(), &LinkInterface::bytesReceived);
    mavlink_message_t request{};
    mavlink_msg_mission_count_pack_chan(255, 190, _link->mavlinkChannel(), &request, _link->vehicleId(),
                                        MAV_COMP_ID_AUTOPILOT1, 2, MAV_MISSION_TYPE_FENCE, 0);
    _link->_missionItemHandler->handleMavlinkMessage(request);
    QCOMPARE(spy.count(), 1);
    _link->resetMissionItemHandler();
    spy.clear();
    mavlink_mission_item_int_t item{};
    item.target_system = _link->vehicleId();
    item.target_component = MAV_COMP_ID_AUTOPILOT1;
    item.mission_type = MAV_MISSION_TYPE_FENCE;
    mavlink_msg_mission_item_int_encode_chan(255, 190, _link->mavlinkChannel(), &request, &item);
    _link->_missionItemHandler->handleMavlinkMessage(request);
    QCOMPARE(spy.count(), 0);
    mavlink_msg_mission_request_list_pack_chan(255, 190, _link->mavlinkChannel(), &request, _link->vehicleId(),
                                               MAV_COMP_ID_AUTOPILOT1, MAV_MISSION_TYPE_FENCE);
    _link->_missionItemHandler->handleMavlinkMessage(request);
    QCOMPARE(spy.count(), 1);
    const mavlink_message_t response = decodeResponse(spy.takeFirst());
    mavlink_mission_count_t count{};
    mavlink_msg_mission_count_decode(&response, &count);
    QCOMPARE(count.count, uint16_t(0));
}

void MockLinkProtocolTest::_invalidMissionTypePreservesUpload_data()
{
    QTest::addColumn<int>("messageId");
    QTest::newRow("count") << MAVLINK_MSG_ID_MISSION_COUNT;
    QTest::newRow("list") << MAVLINK_MSG_ID_MISSION_REQUEST_LIST;
    QTest::newRow("item-request") << MAVLINK_MSG_ID_MISSION_REQUEST_INT;
    QTest::newRow("clear") << MAVLINK_MSG_ID_MISSION_CLEAR_ALL;
}

void MockLinkProtocolTest::_invalidMissionTypePreservesUpload()
{
    QFETCH(int, messageId);
    QSignalSpy spy(_link.get(), &LinkInterface::bytesReceived);
    mavlink_message_t request{};
    mavlink_msg_mission_count_pack_chan(255, 190, _link->mavlinkChannel(), &request, _link->vehicleId(),
                                        MAV_COMP_ID_AUTOPILOT1, 2, MAV_MISSION_TYPE_FENCE, 0);
    _link->_missionItemHandler->handleMavlinkMessage(request);
    QCOMPARE(spy.count(), 1);
    spy.clear();
    constexpr uint8_t invalidType = 4;
    switch (messageId) {
        case MAVLINK_MSG_ID_MISSION_COUNT:
            mavlink_msg_mission_count_pack_chan(255, 190, _link->mavlinkChannel(), &request, _link->vehicleId(),
                                                MAV_COMP_ID_AUTOPILOT1, 2, invalidType, 0);
            break;
        case MAVLINK_MSG_ID_MISSION_REQUEST_LIST:
            mavlink_msg_mission_request_list_pack_chan(255, 190, _link->mavlinkChannel(), &request, _link->vehicleId(),
                                                       MAV_COMP_ID_AUTOPILOT1, invalidType);
            break;
        case MAVLINK_MSG_ID_MISSION_REQUEST_INT:
            mavlink_msg_mission_request_int_pack_chan(255, 190, _link->mavlinkChannel(), &request, _link->vehicleId(),
                                                      MAV_COMP_ID_AUTOPILOT1, 0, invalidType);
            break;
        case MAVLINK_MSG_ID_MISSION_CLEAR_ALL:
            mavlink_msg_mission_clear_all_pack_chan(255, 190, _link->mavlinkChannel(), &request, _link->vehicleId(),
                                                    MAV_COMP_ID_AUTOPILOT1, invalidType);
            break;
    }
    _link->_missionItemHandler->handleMavlinkMessage(request);
    QCOMPARE(spy.count(), 1);
    mavlink_message_t response = decodeResponse(spy.takeFirst());
    QCOMPARE(response.msgid, uint32_t(MAVLINK_MSG_ID_MISSION_ACK));
    mavlink_mission_ack_t ack{};
    mavlink_msg_mission_ack_decode(&response, &ack);
    QCOMPARE(ack.type, uint8_t(MAV_MISSION_UNSUPPORTED));
    QCOMPARE(ack.mission_type, invalidType);

    mavlink_mission_item_int_t item{};
    item.target_system = _link->vehicleId();
    item.target_component = MAV_COMP_ID_AUTOPILOT1;
    item.mission_type = MAV_MISSION_TYPE_FENCE;
    mavlink_msg_mission_item_int_encode_chan(255, 190, _link->mavlinkChannel(), &request, &item);
    _link->_missionItemHandler->handleMavlinkMessage(request);
    QCOMPARE(spy.count(), 1);
    response = decodeResponse(spy.takeFirst());
    QCOMPARE(response.msgid, uint32_t(MAVLINK_MSG_ID_MISSION_REQUEST_INT));
    mavlink_mission_request_int_t next{};
    mavlink_msg_mission_request_int_decode(&response, &next);
    QCOMPARE(next.seq, uint16_t(1));
    QCOMPARE(next.mission_type, uint8_t(MAV_MISSION_TYPE_FENCE));
}

void MockLinkProtocolTest::_channelAllocationRollback()
{
    LinkManager* manager = LinkManager::instance();
    QVERIFY(manager);
    ignoreLogMessage("Comms.LinkManager", QtWarningMsg,
                     QRegularExpression("allocateMavlinkChannel: all channels reserved!"));
    QList<uint8_t> reserved;
    // Return every reserved channel even if a test assertion fails.
    const auto releaseChannels = qScopeGuard([&]() {
        for (const uint8_t channel : reserved) {
            manager->freeMavlinkChannel(channel);
        }
    });
    for (;;) {
        const uint8_t channel = manager->allocateMavlinkChannel();
        if (channel == LinkManager::invalidMavlinkChannel()) {
            break;
        }
        reserved.append(channel);
    }
    QVERIFY(reserved.size() >= 2);
    manager->freeMavlinkChannel(reserved.takeLast());
    manager->freeMavlinkChannel(reserved.takeLast());
    SharedLinkConfigurationPtr config = std::make_shared<MockConfiguration>(QStringLiteral("Exhaustion regression"));
    MockLink second(config);
    second.disconnect();
    expectLogMessage("Comms.MockLink.MockLink", QtWarningMsg,
                     QRegularExpression("_allocateMavlinkChannel vehicle failed"));
    QVERIFY(!second._allocateMavlinkChannel());
    verifyExpectedLogMessage();
    const uint8_t first = manager->allocateMavlinkChannel();
    QVERIFY(first != LinkManager::invalidMavlinkChannel());
    reserved.append(first);
    const uint8_t other = manager->allocateMavlinkChannel();
    QVERIFY(other != LinkManager::invalidMavlinkChannel());
    reserved.append(other);
    second._freeMavlinkChannel();
    // A repeated cleanup must not release channels that now belong to someone
    // else.
    QCOMPARE(manager->allocateMavlinkChannel(), LinkManager::invalidMavlinkChannel());
}

UT_REGISTER_TEST(MockLinkProtocolTest, TestLabel::Unit, TestLabel::Comms)
