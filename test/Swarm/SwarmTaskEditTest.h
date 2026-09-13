#pragma once
#include <memory>

#include "BaseClasses/VehicleTest.h"
class SwarmManager;

class SwarmTaskEditTest : public VehicleTest
{
    Q_OBJECT
public:
    using VehicleTest::VehicleTest;
    ~SwarmTaskEditTest() override;
private slots:
    void init() override;
    void cleanup() override;
    void pendingTaskModelMustExposeStagesActionsAndCondition();
    void editingStageActionMustRevalidateAndApply();
    void invalidStageActionEditMustLeaveTaskUnchanged();
    void editingSetSelectionMustBeRejected();
    void outOfRangeStageOrActionIndexMustBeRejected();
    void editingStageConditionMustApply();
    void changingConditionToManualMustApply();
    void invalidConditionEditMustLeaveTaskUnchanged();
    void typoKeyEditMustReportNoChange();

private:
    void _stagePendingTask();
    std::unique_ptr<SwarmManager> _manager;
};
