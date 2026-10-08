#pragma once

#include "UnitTest.h"

class FormationLegPlannerTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _localOffsetMatchesBearing();
    void _closedFormSeparationMatchesSampling();
    void _assignmentMatchesBruteForce();
    void _assignmentRejectsInvalidInput();
    void _legHeadingKeepsOrientationForShortMoves();

    void _pureTranslationKeepsShape();
    void _routePlansOneLegPerWaypoint();
    void _largeTurnSplitsIntoSubLegs();
    void _layersWhenNoSplitSeparates();
    void _rejectsEndSlotsCloserThanMinimum();
    void _closePairMayOnlyDiverge();
    void _spawnLineReshapeIsPlanned();
    void _keepsCurrentAssignmentWhenNearlyOptimal();
    void _crossingCurrentAssignmentFallsBackToOptimal();
    void _reservedSlotsStayWithTheirHolders();
    void _pathOutsideAreaRejected();
    void _airborneObstacleKeepsPathsClear();
    void _stationaryPairsDoNotBlockPlan();
    void _invalidInputsFailClosed();

    void _trackerSynchronizesSpeeds();
    void _trackerClampsSpeedToCeiling();
    void _trackerCompletesAndTimesOut();
    void _trackerContinuesWithoutRemovedMember();
    void _trackerRestartWithOwnLegKeepsMembers();
    void _trackerSkipsArrivedMemberWhileAnotherLags();
    void _trackerExcludesNegligibleMemberFromConsensus();
    void _trackerArrivalRequiresVerticalMatch();
    void _trackerIgnoresMemberWithInvalidAltitude();
    void _trackerAbortsWhenDesynchronized();

    void _missionWaypointsForFormation();
};
