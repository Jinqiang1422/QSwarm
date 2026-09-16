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
    void modelAbortMustRequireConfirmation();
    void confirmedModelAbortMustStopTheTask();

private:
    /// 启动一个操作员亲自确认的多阶段任务（complete_when=manual，不会自行结束）
    void _startOperatorTask();

    std::unique_ptr<SwarmManager> _manager;
};
