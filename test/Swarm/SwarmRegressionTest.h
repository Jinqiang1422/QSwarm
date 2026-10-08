#pragma once

#include "SwarmVehicleTest.h"

class SwarmRegressionTest : public SwarmVehicleTest
{
    Q_OBJECT
public:
    using SwarmVehicleTest::SwarmVehicleTest;
private slots:
    void queuedArmingMustBeCancelledByPause();
    void queuedArmingMustBeCancelledByPause_data();
    void taskAbortMustStopFormation();
    void neverStartedMissionMustNotComplete();
    void invalidTemplateNumberMustBeRejected();
    void coverageRoundTripMustRetainTakeoff();
    void coverageRoundTripMustRetainTakeoff_data();
    void coverageLeavesHeadingToVehicle();
    void coverageLeavesHeadingToVehicle_data();
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
    void px4FlightModesRetainMeaningAcrossProtocolVersions();
    void activeFormationEditsUpdateTargets();
    void guidanceTargetTracksSlotMotion();
    void formationSkipsFollowerAwaitingRepositionAck();
    void formationRebuildsSlotsWhenMemberLacksOne();
};
