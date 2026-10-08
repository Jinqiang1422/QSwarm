#pragma once

#include "SwarmVehicleTest.h"

/// 编队航段（设计 2026-10-05-synchronized-formation-legs）的多机集成测试
class SwarmFormationTest : public SwarmVehicleTest
{
    Q_OBJECT

public:
    using SwarmVehicleTest::SwarmVehicleTest;

private slots:
    void operatorGotoMovesWholeFormation();
    void formationRouteFromLeaderMission();
    void formationGotoRefusedOutsideFence();
    void formationGotoOnUnguidedFollowerIsNative();
    void px4RepositionCarriesGroundSpeed();
    void formationStartConvergesInSync();
    void formationReshapeWaitsForLeaderToStop();
    void formationRoutePreviewSkipsFollowersWithoutSlot();
    void formationLegRequiresLeaderLease();
    void formationSkipsMemberInFailsafe();
    void formationLegAbortsWhenRepositionRejected();
    void formationDropsFollowerEnteringFailsafeMidTransit();
    void formationMemberLeavesLegOnModeChange();
    void formationRejectsLegWhenLeaderInFailsafe();
    void formationStrandedLeaderIsNotGuided();
    void formationGotoOnStrandedLeaderIsNative();
};
