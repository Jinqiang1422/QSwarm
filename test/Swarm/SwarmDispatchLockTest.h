#pragma once
#include <memory>

#include "BaseClasses/VehicleTest.h"
class SwarmManager;

class SwarmDispatchLockTest : public VehicleTest
{
    Q_OBJECT
public:
    using VehicleTest::VehicleTest;
    ~SwarmDispatchLockTest() override;
private slots:
    void init() override;
    void cleanup() override;
    void lockedBuildMustReportNoAutodispatch();
    void autonomyLevelMustBePinnedToSupervised();
    void persistedAutonomyLevelMustBeIgnored();
    void requireConfirmMustBePinnedOn();
    void harmlessAiPlanMustNotAutoDispatch();
    void confirmedAiPlanMustDispatch();
    void operatorTaskMustStillRun();

private:
    std::unique_ptr<SwarmManager> _manager;
};
