#pragma once

#include <memory>

#include "MockLink.h"
#include "UnitTest.h"

class MockLinkProtocolTest : public UnitTest
{
    Q_OBJECT

private slots:
    void init() override;
    void cleanup() override;
    void _logReadPreservesBytes();
    void _logReadFailure();
    void _invalidParameterRequests();
    void _ftpReadSession_data();
    void _ftpReadSession();
    void _ftpReadFailure_data();
    void _ftpReadFailure();
    void _invalidMissionRequestPreservesItems();
    void _missionResetCancelsUpload();
    void _invalidMissionTypePreservesUpload_data();
    void _invalidMissionTypePreservesUpload();
    void _channelAllocationRollback();

private:
    std::unique_ptr<MockLink> _link;
};
