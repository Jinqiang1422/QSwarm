#pragma once
#include <QtCore/QPointer>
#include <memory>

#include "BaseClasses/VehicleTest.h"
class SwarmManager;

class SwarmSafetyTest : public VehicleTest
{
    Q_OBJECT
public:
    using VehicleTest::VehicleTest;
    ~SwarmSafetyTest() override;
private slots:
    void init() override;
    void cleanup() override;
    void policyRevocationMustStopQueuedAutomaticArm_data();
    void policyRevocationMustStopQueuedAutomaticArm();
    void queuedTakeoffMustRecheckChangedAltitudeLimit();
    void existingMissionMustRecheckChangedAltitudeLimit();
    void clearingBusyMissionMustNotReportSuccess();
    void clearMustVerifyEmptyReadback();
    void uploadNotificationMustNotCompleteNewReadback();
    void missionSnapshotValidation();
    void perceptionQueueMustRemainBounded();
    void terminalNotificationMustPreserveNewTask_data();
    void terminalNotificationMustPreserveNewTask();
    void invalidCoordinateMustNotEnterCoverageMap();
    void targetLeavingIncludeFenceMustRaiseAlert();
    void continuousTrackHistoryMustExpire();

private:
    void addSecondVehicle();
    void writeFence(const QByteArray& json);
    std::unique_ptr<SwarmManager> _manager;
    QPointer<MockLink> _secondaryLink;
    QString _configPath;
    QByteArray _previousConfig;
    bool _hadConfig = false;
};
