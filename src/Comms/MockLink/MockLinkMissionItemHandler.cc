#include "MockLinkMissionItemHandler.h"

#include "MAVLinkProtocol.h"
#include "MockLink.h"
#include "QGCLoggingCategory.h"

QGC_LOGGING_CATEGORY(MockLinkMissionItemHandlerLog, "Comms.MockLink.MockLinkMissionItemHandler")

namespace {
void sendMissionAck(MockLink* link, MAV_MISSION_RESULT result, uint8_t missionType)
{
    if (!link) {
        return;
    }
    mavlink_message_t message{};
    mavlink_msg_mission_ack_pack_chan(link->vehicleId(), MAV_COMP_ID_AUTOPILOT1, link->outgoingMavlinkChannel(),
                                      &message, MAVLinkProtocol::instance()->getSystemId(),
                                      MAVLinkProtocol::getComponentId(), result, missionType, 0);
    link->respondWithMavlinkMessage(message);
}
}  // namespace

MockLinkMissionItemHandler::MockLinkMissionItemHandler(MockLink* mockLink) : QObject(mockLink), _mockLink(mockLink)
{
    // qCDebug(MockLinkMissionItemHandlerLog) << Q_FUNC_INFO << this;

    _missionItemResponseTimer.setSingleShot(true);

    (void) connect(&_missionItemResponseTimer, &QTimer::timeout, this,
                   &MockLinkMissionItemHandler::_missionItemResponseTimeout);
}

MockLinkMissionItemHandler::~MockLinkMissionItemHandler()
{
    // qCDebug(MockLinkMissionItemHandlerLog) << Q_FUNC_INFO << this;
}

void MockLinkMissionItemHandler::_startMissionItemResponseTimer()
{
    _missionItemResponseTimer.start(500);
}

void MockLinkMissionItemHandler::loadSimpleMultirotorMission()
{
    _missionItems.clear();

    constexpr double homeLatitude = 47.397;
    constexpr double homeLongitude = 8.5455;
    constexpr float relativeAltitude = 50.0f;

    const auto makeItem = [](uint16_t seq, uint16_t command, bool current, double latitude, double longitude,
                             float altitude) {
        mavlink_mission_item_int_t item{};
        item.seq = seq;
        item.frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
        item.command = command;
        item.current = current ? 1 : 0;
        item.autocontinue = 1;
        item.x = static_cast<int32_t>(latitude * 1e7);
        item.y = static_cast<int32_t>(longitude * 1e7);
        item.z = altitude;
        item.mission_type = MAV_MISSION_TYPE_MISSION;
        return item;
    };

    // Seq 0: Takeoff
    _missionItems[0] = makeItem(0, MAV_CMD_NAV_TAKEOFF, true, homeLatitude, homeLongitude, relativeAltitude);
    // Seq 1: Waypoint
    _missionItems[1] =
        makeItem(1, MAV_CMD_NAV_WAYPOINT, false, homeLatitude + 0.001, homeLongitude + 0.001, relativeAltitude);
    // Seq 2: Return to launch
    _missionItems[2] = makeItem(2, MAV_CMD_NAV_RETURN_TO_LAUNCH, false, 0.0, 0.0, 0.0);

    qCDebug(MockLinkMissionItemHandlerLog) << "loadSimpleMultirotorMission seeded" << _missionItems.count() << "items";
}

bool MockLinkMissionItemHandler::handleMavlinkMessage(const mavlink_message_t& msg)
{
    if (!_mockLink) {
        return false;
    }
    switch (msg.msgid) {
        case MAVLINK_MSG_ID_MISSION_REQUEST_LIST:
            _handleMissionRequestList(msg);
            break;
        case MAVLINK_MSG_ID_MISSION_REQUEST_INT:
            _handleMissionRequest(msg);
            break;
        case MAVLINK_MSG_ID_MISSION_ITEM_INT:
            _handleMissionItem(msg);
            break;
        case MAVLINK_MSG_ID_MISSION_COUNT:
            _handleMissionCount(msg);
            break;
        case MAVLINK_MSG_ID_MISSION_ACK:
            // Acks are received back for each MISSION_ITEM message
            break;
        case MAVLINK_MSG_ID_MISSION_SET_CURRENT:
            // Sets the currently active mission item
            break;
        case MAVLINK_MSG_ID_MISSION_CLEAR_ALL:
            _handleMissionClearAll(msg);
            break;
        default:
            return false;
    }

    return true;
}

void MockLinkMissionItemHandler::_handleMissionClearAll(const mavlink_message_t& msg)
{
    mavlink_mission_clear_all_t clearAll{};
    mavlink_msg_mission_clear_all_decode(&msg, &clearAll);

    if (clearAll.target_system != _mockLink->vehicleId() ||
        (clearAll.target_component != MAV_COMP_ID_ALL && clearAll.target_component != MAV_COMP_ID_AUTOPILOT1)) {
        return;
    }

    const auto requestType = static_cast<MAV_MISSION_TYPE>(clearAll.mission_type);
    qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionClearAll" << requestType;

    switch (requestType) {
        case MAV_MISSION_TYPE_MISSION:
            _missionItems.clear();
            break;
        case MAV_MISSION_TYPE_FENCE:
            _fenceItems.clear();
            break;
        case MAV_MISSION_TYPE_RALLY:
            _rallyItems.clear();
            break;
        case MAV_MISSION_TYPE_ALL:
            _missionItems.clear();
            _fenceItems.clear();
            _rallyItems.clear();
            break;
        default:
            sendMissionAck(_mockLink, MAV_MISSION_UNSUPPORTED, clearAll.mission_type);
            return;
    }

    if (requestType == _requestType || requestType == MAV_MISSION_TYPE_ALL) {
        _missionItemResponseTimer.stop();
        _writeSequenceCount = 0;
        _writeSequenceIndex = 0;
    }
    sendMissionAck(_mockLink, MAV_MISSION_ACCEPTED, clearAll.mission_type);
}

void MockLinkMissionItemHandler::_handleMissionRequestList(const mavlink_message_t& msg)
{
    qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionRequestList read sequence";

    mavlink_mission_request_list_t request{};
    mavlink_msg_mission_request_list_decode(&msg, &request);
    if (request.target_system != _mockLink->vehicleId() ||
        (request.target_component != MAV_COMP_ID_ALL && request.target_component != MAV_COMP_ID_AUTOPILOT1)) {
        return;
    }

    _failReadRequest1FirstResponse = true;

    if (_failureMode == FailReadRequestListNoResponse) {
        qCDebug(MockLinkMissionItemHandlerLog)
            << "_handleMissionRequestList not responding due to failure mode FailReadRequestListNoResponse";
        return;
    }

    if ((_failureMode == FailReadRequestListFirstResponse) && _failReadRequestListFirstResponse) {
        _failReadRequestListFirstResponse = false;
        qCDebug(MockLinkMissionItemHandlerLog)
            << "_handleMissionRequestList not responding due to failure mode FailReadRequestListFirstResponse";
        return;
    }

    _failReadRequestListFirstResponse = true;

    const auto requestType = static_cast<MAV_MISSION_TYPE>(request.mission_type);

    int itemCount;
    switch (requestType) {
        case MAV_MISSION_TYPE_MISSION:
            itemCount = _missionItems.count();
            if (itemCount == 0 && _sendHomePositionOnEmptyList) {
                itemCount = 1;
            }
            break;
        case MAV_MISSION_TYPE_FENCE:
            itemCount = _fenceItems.count();
            break;
        case MAV_MISSION_TYPE_RALLY:
            itemCount = _rallyItems.count();
            break;
        default:
            sendMissionAck(_mockLink, MAV_MISSION_UNSUPPORTED, request.mission_type);
            return;
    }

    _requestListCounts[requestType]++;
    mavlink_message_t responseMsg{};
    (void) mavlink_msg_mission_count_pack_chan(_mockLink->vehicleId(), MAV_COMP_ID_AUTOPILOT1,
                                               _mockLink->outgoingMavlinkChannel(),
                                               &responseMsg,  // Outgoing message
                                               msg.sysid,     // Target is original sender
                                               msg.compid,    // Target is original sender
                                               itemCount,     // Number of mission items
                                               requestType, 0);

    _mockLink->respondWithMavlinkMessage(responseMsg);
}

void MockLinkMissionItemHandler::_handleMissionRequest(const mavlink_message_t& msg)
{
    qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionRequest read sequence";

    mavlink_mission_request_int_t request{};
    mavlink_msg_mission_request_int_decode(&msg, &request);

    if (request.target_system != _mockLink->vehicleId() ||
        (request.target_component != MAV_COMP_ID_ALL && request.target_component != MAV_COMP_ID_AUTOPILOT1)) {
        return;
    }

    const auto requestType = static_cast<MAV_MISSION_TYPE>(request.mission_type);
    bool sequenceExists = false;
    switch (requestType) {
        case MAV_MISSION_TYPE_MISSION:
            sequenceExists = _missionItems.contains(request.seq) ||
                             (_missionItems.isEmpty() && _sendHomePositionOnEmptyList && request.seq == 0);
            break;
        case MAV_MISSION_TYPE_FENCE:
            sequenceExists = _fenceItems.contains(request.seq);
            break;
        case MAV_MISSION_TYPE_RALLY:
            sequenceExists = _rallyItems.contains(request.seq);
            break;
        default:
            sendMissionAck(_mockLink, MAV_MISSION_UNSUPPORTED, request.mission_type);
            return;
    }
    if (!sequenceExists) {
        sendMissionAck(_mockLink, MAV_MISSION_INVALID_SEQUENCE, request.mission_type);
        return;
    }

    if ((_failureMode == FailReadRequest0NoResponse) && (request.seq == 0)) {
        qCDebug(MockLinkMissionItemHandlerLog)
            << "_handleMissionRequest not responding due to failure mode FailReadRequest0NoResponse";
        return;
    }

    if ((_failureMode == FailReadRequest1NoResponse) && (request.seq == 1)) {
        qCDebug(MockLinkMissionItemHandlerLog)
            << "_handleMissionRequest not responding due to failure mode FailReadRequest1NoResponse";
        return;
    }

    if ((_failureMode == FailReadRequest1FirstResponse) && (request.seq == 1) && _failReadRequest1FirstResponse) {
        _failReadRequest1FirstResponse = false;
        qCDebug(MockLinkMissionItemHandlerLog)
            << "_handleMissionRequest not responding due to failure mode FailReadRequest1FirstResponse";
        return;
    }

    // FIXME: Track whether all items are requested, or requested in sequence

    if (((_failureMode == FailReadRequest0IncorrectSequence) && (request.seq == 0)) ||
        ((_failureMode == FailReadRequest1IncorrectSequence) && (request.seq == 1))) {
        // Send back the incorrect sequence number
        qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionRequest sending bad sequence number";
        request.seq++;
    }

    if (((_failureMode == FailReadRequest0ErrorAck) && (request.seq == 0)) ||
        ((_failureMode == FailReadRequest1ErrorAck) && (request.seq == 1))) {
        sendMissionAck(_mockLink, _failureAckResult, request.mission_type);
        return;
    }

    mavlink_mission_item_int_t missionItemInt{};

    switch (request.mission_type) {
        case MAV_MISSION_TYPE_MISSION:
            if (_missionItems.isEmpty() && _sendHomePositionOnEmptyList) {
                (void) memset(&missionItemInt, 0, sizeof(missionItemInt));
                missionItemInt.frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
                missionItemInt.command = MAV_CMD_NAV_WAYPOINT;
                missionItemInt.autocontinue = true;
            } else {
                missionItemInt = _missionItems.value(request.seq);
            }
            break;
        case MAV_MISSION_TYPE_FENCE:
            missionItemInt = _fenceItems.value(request.seq);
            break;
        case MAV_MISSION_TYPE_RALLY:
            missionItemInt = _rallyItems.value(request.seq);
            break;
        default:
            sendMissionAck(_mockLink, MAV_MISSION_UNSUPPORTED, request.mission_type);
            return;
    }

    mavlink_message_t responseMsg{};
    (void) mavlink_msg_mission_item_int_pack_chan(
        _mockLink->vehicleId(), MAV_COMP_ID_AUTOPILOT1, _mockLink->outgoingMavlinkChannel(),
        &responseMsg,  // Outgoing message
        msg.sysid,     // Target is original sender
        msg.compid,    // Target is original sender
        request.seq,   // Index of mission item being sent
        missionItemInt.frame, missionItemInt.command, missionItemInt.current, missionItemInt.autocontinue,
        missionItemInt.param1, missionItemInt.param2, missionItemInt.param3, missionItemInt.param4, missionItemInt.x,
        missionItemInt.y, missionItemInt.z, requestType);

    _mockLink->respondWithMavlinkMessage(responseMsg);
}

void MockLinkMissionItemHandler::_handleMissionCount(const mavlink_message_t& msg)
{
    mavlink_mission_count_t missionCount{};
    mavlink_msg_mission_count_decode(&msg, &missionCount);
    if (missionCount.target_system != _mockLink->vehicleId() ||
        (missionCount.target_component != MAV_COMP_ID_ALL && missionCount.target_component != MAV_COMP_ID_AUTOPILOT1)) {
        return;
    }

    const auto requestType = static_cast<MAV_MISSION_TYPE>(missionCount.mission_type);
    if (requestType != MAV_MISSION_TYPE_MISSION && requestType != MAV_MISSION_TYPE_FENCE &&
        requestType != MAV_MISSION_TYPE_RALLY) {
        sendMissionAck(_mockLink, MAV_MISSION_UNSUPPORTED, missionCount.mission_type);
        return;
    }
    _requestType = requestType;
    _missionItemResponseTimer.stop();
    _writeSequenceCount = missionCount.count;
    _writeSequenceIndex = 0;

    qCDebug(MockLinkMissionItemHandlerLog)
        << "_handleMissionCount write sequence _writeSequenceCount:" << _writeSequenceCount;

    switch (missionCount.mission_type) {
        case MAV_MISSION_TYPE_MISSION:
            _missionItems.clear();
            break;
        case MAV_MISSION_TYPE_FENCE:
            _fenceItems.clear();
            break;
        case MAV_MISSION_TYPE_RALLY:
            _rallyItems.clear();
            break;
    }

    if (_writeSequenceCount == 0) {
        _sendAck(MAV_MISSION_ACCEPTED);
        return;
    }

    if (_failureMode == FailWriteMissionCountNoResponse) {
        qCDebug(MockLinkMissionItemHandlerLog)
            << "_handleMissionCount not responding due to failure mode FailWriteMissionCountNoResponse";
        return;
    }

    if (_failureMode == FailWriteMissionCountFirstResponse && _failWriteMissionCountFirstResponse) {
        _failWriteMissionCountFirstResponse = false;
        qCDebug(MockLinkMissionItemHandlerLog)
            << "_handleMissionCount not responding due to failure mode FailWriteMissionCountNoResponse";
        return;
    }

    _failWriteMissionCountFirstResponse = true;
    _writeSequenceIndex = 0;
    _requestNextMissionItem(_writeSequenceIndex);
}

void MockLinkMissionItemHandler::_requestNextMissionItem(int sequenceNumber)
{
    qCDebug(MockLinkMissionItemHandlerLog)
        << "write sequence sequenceNumber:" << sequenceNumber << "_failureMode:" << _failureMode;

    if ((_failureMode == FailWriteRequest1NoResponse) && (sequenceNumber == 1)) {
        qCDebug(MockLinkMissionItemHandlerLog)
            << "_requestNextMissionItem not responding due to failure mode FailWriteRequest1NoResponse";
        return;
    }

    if (!_mockLink || sequenceNumber < 0 || sequenceNumber >= _writeSequenceCount) {
        qCWarning(MockLinkMissionItemHandlerLog)
            << "_requestNextMissionItem requested seqeuence number > write count sequenceNumber::_writeSequenceCount"
            << sequenceNumber << _writeSequenceCount;
        return;
    }

    if (((_failureMode == FailWriteRequest0IncorrectSequence) && (sequenceNumber == 0)) ||
        ((_failureMode == FailWriteRequest1IncorrectSequence) && (sequenceNumber == 1))) {
        sequenceNumber++;
    }

    if (((_failureMode == FailWriteRequest0ErrorAck) && (sequenceNumber == 0)) ||
        ((_failureMode == FailWriteRequest1ErrorAck) && (sequenceNumber == 1))) {
        qCDebug(MockLinkMissionItemHandlerLog) << "_requestNextMissionItem sending ack error due to failure mode";
        _sendAck(_failureAckResult);
        return;
    }

    mavlink_message_t message{};
    (void) mavlink_msg_mission_request_int_pack_chan(
        _mockLink->vehicleId(), MAV_COMP_ID_AUTOPILOT1, _mockLink->outgoingMavlinkChannel(), &message,
        MAVLinkProtocol::instance()->getSystemId(), MAVLinkProtocol::getComponentId(), sequenceNumber, _requestType);
    _mockLink->respondWithMavlinkMessage(message);

    // If response with Mission Item doesn't come before timer fires it's an error
    _startMissionItemResponseTimer();
}

void MockLinkMissionItemHandler::_sendAck(MAV_MISSION_RESULT ackType) const
{
    qCDebug(MockLinkMissionItemHandlerLog) << "_sendAck write sequence complete ackType:" << ackType;
    sendMissionAck(_mockLink, ackType, _requestType);
}

void MockLinkMissionItemHandler::_handleMissionItem(const mavlink_message_t& msg)
{
    qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionItem write sequence";

    mavlink_mission_item_int_t missionItemInt{};
    mavlink_msg_mission_item_int_decode(&msg, &missionItemInt);

    if (missionItemInt.target_system != _mockLink->vehicleId() ||
        (missionItemInt.target_component != MAV_COMP_ID_ALL &&
         missionItemInt.target_component != MAV_COMP_ID_AUTOPILOT1) ||
        missionItemInt.mission_type != _requestType) {
        return;
    }
    if (missionItemInt.seq != _writeSequenceIndex || _writeSequenceIndex >= _writeSequenceCount) {
        _sendAck(MAV_MISSION_INVALID_SEQUENCE);
        return;
    }
    _missionItemResponseTimer.stop();

    const MAV_MISSION_TYPE missionType = static_cast<MAV_MISSION_TYPE>(missionItemInt.mission_type);
    const uint16_t seq = missionItemInt.seq;

    switch (missionType) {
        case MAV_MISSION_TYPE_MISSION:
            _missionItems[seq] = missionItemInt;
            break;
        case MAV_MISSION_TYPE_FENCE:
            _fenceItems[seq] = missionItemInt;
            break;
        case MAV_MISSION_TYPE_RALLY:
            _rallyItems[seq] = missionItemInt;
            break;
        case MAV_MISSION_TYPE_ENUM_END:
        case MAV_MISSION_TYPE_ALL:
            qCWarning(MockLinkMissionItemHandlerLog) << "Internal error";
            break;
    }

    _writeSequenceIndex++;
    if (_writeSequenceIndex < _writeSequenceCount) {
        if ((_failureMode == FailWriteFinalAckMissingRequests) && (_writeSequenceIndex == 3)) {
            // Send MAV_MISSION_ACCEPTED ack too early
            _sendAck(MAV_MISSION_ACCEPTED);
        } else {
            _requestNextMissionItem(_writeSequenceIndex);
        }

        return;
    }

    if (_failureMode != FailWriteFinalAckNoResponse) {
        MAV_MISSION_RESULT ack = MAV_MISSION_ACCEPTED;

        if (_failureMode == FailWriteFinalAckErrorAck) {
            ack = MAV_MISSION_ERROR;
        }

        _sendAck(ack);
    }
}

void MockLinkMissionItemHandler::_missionItemResponseTimeout()
{
    qCWarning(MockLinkMissionItemHandlerLog) << "Timeout waiting for next MISSION_ITEM_INT";
    _writeSequenceCount = 0;
    _writeSequenceIndex = 0;
    _sendAck(MAV_MISSION_ERROR);
}

void MockLinkMissionItemHandler::sendUnexpectedMissionAck(MAV_MISSION_RESULT ackType)
{
    _sendAck(ackType);
}

void MockLinkMissionItemHandler::sendUnexpectedMissionItem()
{
    if (!_mockLink) {
        return;
    }
    mavlink_mission_item_int_t item{};
    item.target_system = MAVLinkProtocol::instance()->getSystemId();
    item.target_component = MAVLinkProtocol::getComponentId();
    item.command = MAV_CMD_NAV_WAYPOINT;
    item.frame = MAV_FRAME_GLOBAL_RELATIVE_ALT_INT;
    item.mission_type = MAV_MISSION_TYPE_MISSION;
    mavlink_message_t message{};
    mavlink_msg_mission_item_int_encode_chan(_mockLink->vehicleId(), MAV_COMP_ID_AUTOPILOT1,
                                             _mockLink->outgoingMavlinkChannel(), &message, &item);
    _mockLink->respondWithMavlinkMessage(message);
}

void MockLinkMissionItemHandler::sendUnexpectedMissionRequest()
{
    if (!_mockLink) {
        return;
    }
    mavlink_message_t message{};
    mavlink_msg_mission_request_int_pack_chan(
        _mockLink->vehicleId(), MAV_COMP_ID_AUTOPILOT1, _mockLink->outgoingMavlinkChannel(), &message,
        MAVLinkProtocol::instance()->getSystemId(), MAVLinkProtocol::getComponentId(), 0, MAV_MISSION_TYPE_MISSION);
    _mockLink->respondWithMavlinkMessage(message);
}

void MockLinkMissionItemHandler::setFailureMode(FailureMode_t failureMode, MAV_MISSION_RESULT failureAckResult)
{
    _failureMode = failureMode;
    _failureAckResult = failureAckResult;
}

void MockLinkMissionItemHandler::shutdown()
{
    _missionItemResponseTimer.stop();
}

void MockLinkMissionItemHandler::reset()
{
    _missionItemResponseTimer.stop();
    _missionItems.clear();
    _fenceItems.clear();
    _rallyItems.clear();
    _requestListCounts.clear();
    _writeSequenceCount = 0;
    _writeSequenceIndex = 0;
    _requestType = MAV_MISSION_TYPE_MISSION;
    _failReadRequestListFirstResponse = true;
    _failReadRequest1FirstResponse = true;
    _failWriteMissionCountFirstResponse = true;
}
