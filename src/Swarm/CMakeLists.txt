# ============================================================================
# Swarm Module
# 集群指挥控制：多机批量指令、编队控制、集群任务规划
# 集成方式与 FollowMe 等 C++ 模块一致：源码直接编入主程序目标，
# 其中的 QML_ELEMENT 类随 QGroundControl QML 模块自动注册。
# ============================================================================

target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        FormationController.cc
        FormationController.h
        SwarmAction.h
        SwarmAIAgent.cc
        SwarmAIAgent.h
        SwarmAuditLogger.cc
        SwarmAuditLogger.h
        SwarmAutonomy.h
        SwarmConstraintChecker.cc
        SwarmConstraintChecker.h
        SwarmCOPModel.cc
        SwarmCOPModel.h
        SwarmEventDetector.cc
        SwarmEventDetector.h
        SwarmInputFilter.cc
        SwarmInputFilter.h
        SwarmPerceptionBridge.cc
        SwarmPerceptionBridge.h
        SwarmRehearsalEstimator.cc
        SwarmRehearsalEstimator.h
        SwarmSituationDigest.cc
        SwarmSituationDigest.h
        SwarmTaskAllocator.cc
        SwarmTaskAllocator.h
        SwarmTactics.cc
        SwarmTactics.h
        SwarmTrackFusion.cc
        SwarmTrackFusion.h
        SwarmTypes.h
        SwarmVideoHub.cc
        SwarmVideoHub.h
        SwarmManager.cc
        SwarmManager.h
        SwarmMissionPlanner.cc
        SwarmMissionPlanner.h
        SwarmTaskController.cc
        SwarmTaskController.h
        SwarmTaskLibrary.cc
        SwarmTaskLibrary.h
        SwarmTunnelProtocol.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
