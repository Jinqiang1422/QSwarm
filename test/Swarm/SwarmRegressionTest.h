#pragma once
#include <QtCore/QPointer>
#include <memory>

#include "BaseClasses/VehicleTest.h"
class SwarmManager;

class SwarmRegressionTest : public VehicleTest
{
    Q_OBJECT
public:
    using VehicleTest::VehicleTest;
    ~SwarmRegressionTest() override;
private slots:
    void init() override;
    void cleanup() override;
    void queuedArmingMustBeCancelledByPause();
    void queuedArmingMustBeCancelledByPause_data();
    void taskAbortMustStopFormation();
    void neverStartedMissionMustNotComplete();
    void invalidTemplateNumberMustBeRejected();
    void coverageRoundTripMustRetainTakeoff();
    void coverageRoundTripMustRetainTakeoff_data();
    void missionMustWaitForUploadAcknowledgement();
    void remainingMembersContinueLaterStages();
    void withdrawnGroupDoesNotBlockFutureGroups();
    void allTemplatesUseRuntimeContracts();
    void legacyTemplateMigrationPreservesCustomEdits();
    void missionCompletionRequiresCurrentExecutionEvidence();
    void lateTakeoffAcknowledgementCannotArmWithdrawnTarget();
    void missionReplacementBlocksLaterStart();
    void missionReplacementBlocksLaterStart_data();
    void formationLeaderWithdrawalKeepsRemainingMembers();
    void durationUsesMonotonicDeadline();

private:
    std::unique_ptr<SwarmManager> _manager;
    QPointer<MockLink> _secondaryLink;
    QList<QPointer<MockLink>> _extraLinks;
    void addVehicles(int count);
    QString _configPath;
    QByteArray _previousConfig;
    bool _hadConfig = false;
};
