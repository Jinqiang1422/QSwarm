# ============================================================================
# P1 模块独立单元测试（WP1.6）
# 仅依赖 Qt6 Core/Test，不依赖 QGC 主工程，可在 CI 快速构建运行。
# 构建：cmake -B build && cmake --build build && ctest --test-dir build
# ============================================================================
cmake_minimum_required(VERSION 3.22)
project(swarm_p1_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Positioning Qml Test)

enable_testing()

# 合并进 QGC 后本工程在 swarm_ext/tests/cpp/，src/Swarm 在仓库根(上 3 级)
set(SWARM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/Swarm)

add_executable(
    tst_p1modules
    tst_p1modules.cc
    ${SWARM_SRC}/SwarmAuditLogger.cc
    ${SWARM_SRC}/SwarmAuditLogger.h
    ${SWARM_SRC}/SwarmInputFilter.cc
    ${SWARM_SRC}/SwarmInputFilter.h
    ${SWARM_SRC}/SwarmAutonomy.h
    ${SWARM_SRC}/SwarmAction.h
)
target_include_directories(tst_p1modules PRIVATE ${SWARM_SRC})
target_link_libraries(tst_p1modules PRIVATE Qt6::Core Qt6::Test)

add_test(NAME p1modules COMMAND tst_p1modules)

# --- P2 模块（约束引擎 / 预演估算 / 拍卖分配） ---
add_executable(
    tst_p2modules
    tst_p2modules.cc
    ${SWARM_SRC}/SwarmConstraintChecker.cc
    ${SWARM_SRC}/SwarmConstraintChecker.h
    ${SWARM_SRC}/SwarmRehearsalEstimator.cc
    ${SWARM_SRC}/SwarmRehearsalEstimator.h
    ${SWARM_SRC}/SwarmTaskAllocator.cc
    ${SWARM_SRC}/SwarmTaskAllocator.h
    ${SWARM_SRC}/SwarmTypes.h
    ${SWARM_SRC}/SwarmAction.h
)
target_include_directories(tst_p2modules PRIVATE ${SWARM_SRC})
target_link_libraries(tst_p2modules PRIVATE Qt6::Core Qt6::Test)

add_test(NAME p2modules COMMAND tst_p2modules)

# --- P3 模块（航迹融合 / 事件检测 / 态势摘要 / 视频汇聚 / 隧道协议解析） ---
add_executable(
    tst_p3modules
    tst_p3modules.cc
    ${SWARM_SRC}/SwarmTrackFusion.cc
    ${SWARM_SRC}/SwarmTrackFusion.h
    ${SWARM_SRC}/SwarmEventDetector.cc
    ${SWARM_SRC}/SwarmEventDetector.h
    ${SWARM_SRC}/SwarmSituationDigest.cc
    ${SWARM_SRC}/SwarmSituationDigest.h
    ${SWARM_SRC}/SwarmVideoHub.cc
    ${SWARM_SRC}/SwarmVideoHub.h
    ${SWARM_SRC}/SwarmTunnelProtocol.h
    ${SWARM_SRC}/SwarmTypes.h
)
target_include_directories(tst_p3modules PRIVATE ${SWARM_SRC})
target_link_libraries(tst_p3modules PRIVATE Qt6::Core Qt6::Test)

add_test(NAME p3modules COMMAND tst_p3modules)

# --- 集群协同战术（协同搜索 / 巡逻 / 跟踪 / 围捕 / 拦截） ---
add_executable(tst_tactics
               tst_tactics.cc ${SWARM_SRC}/SwarmTactics.cc ${SWARM_SRC}/SwarmTactics.h
               ${SWARM_SRC}/SwarmTaskAllocator.cc ${SWARM_SRC}/SwarmTaskAllocator.h ${SWARM_SRC}/SwarmTypes.h
)
target_include_directories(tst_tactics PRIVATE ${SWARM_SRC})
target_link_libraries(tst_tactics PRIVATE Qt6::Core Qt6::Test)

add_test(NAME tactics COMMAND tst_tactics)

# --- 端到端安全闸门链集成测试(防注入 + 约束 + 自主分级) ---
add_executable(
    tst_pipeline
    tst_pipeline.cc
    ${SWARM_SRC}/SwarmInputFilter.cc
    ${SWARM_SRC}/SwarmInputFilter.h
    ${SWARM_SRC}/SwarmConstraintChecker.cc
    ${SWARM_SRC}/SwarmConstraintChecker.h
    ${SWARM_SRC}/SwarmAutonomy.h
    ${SWARM_SRC}/SwarmAction.h
    ${SWARM_SRC}/SwarmTypes.h
    ${SWARM_SRC}/SwarmTrackFusion.cc
    ${SWARM_SRC}/SwarmTrackFusion.h
    ${SWARM_SRC}/SwarmControlLease.cc
    ${SWARM_SRC}/SwarmControlLease.h
)
target_include_directories(tst_pipeline PRIVATE ${SWARM_SRC})
target_link_libraries(tst_pipeline PRIVATE Qt6::Core Qt6::Test)

add_test(NAME pipeline COMMAND tst_pipeline)

# --- 控制租约(per-vehicle 状态机 + 飞行模式释放) ---
add_executable(tst_lease tst_lease.cc ${SWARM_SRC}/SwarmControlLease.cc ${SWARM_SRC}/SwarmControlLease.h)
target_include_directories(tst_lease PRIVATE ${SWARM_SRC})
target_link_libraries(tst_lease PRIVATE Qt6::Core Qt6::Test)
add_test(NAME lease COMMAND tst_lease)

# --- 任务持久化(JSON 原子写 + RUNNING→PAUSED 恢复) ---
add_executable(tst_persistence tst_persistence.cc ${SWARM_SRC}/SwarmPersistence.cc ${SWARM_SRC}/SwarmPersistence.h)
target_include_directories(tst_persistence PRIVATE ${SWARM_SRC})
target_link_libraries(tst_persistence PRIVATE Qt6::Core Qt6::Test)
add_test(NAME persistence COMMAND tst_persistence)

# --- COP 模型 + 实际 UDP 感知桥端到端 ---
add_executable(
    tst_cop
    tst_cop.cc
    ${SWARM_SRC}/SwarmCOPModel.cc
    ${SWARM_SRC}/SwarmCOPModel.h
    ${SWARM_SRC}/SwarmPerceptionBridge.cc
    ${SWARM_SRC}/SwarmPerceptionBridge.h
    ${SWARM_SRC}/SwarmTrackFusion.cc
    ${SWARM_SRC}/SwarmTrackFusion.h
    ${SWARM_SRC}/SwarmTunnelProtocol.h
    ${SWARM_SRC}/SwarmTypes.h
)
target_include_directories(tst_cop PRIVATE ${SWARM_SRC})
target_link_libraries(tst_cop PRIVATE Qt6::Core Qt6::Network Qt6::Positioning Qt6::Qml Qt6::Test)
add_test(NAME cop COMMAND tst_cop)

# --- 本地容量门槛（PT-008 / PT-012 的确定性突发负载部分） ---
add_executable(
    tst_performance
    tst_performance.cc
    ${SWARM_SRC}/SwarmAuditLogger.cc
    ${SWARM_SRC}/SwarmAuditLogger.h
    ${SWARM_SRC}/SwarmCOPModel.cc
    ${SWARM_SRC}/SwarmCOPModel.h
    ${SWARM_SRC}/SwarmTrackFusion.cc
    ${SWARM_SRC}/SwarmTrackFusion.h
    ${SWARM_SRC}/SwarmTypes.h
)
target_include_directories(tst_performance PRIVATE ${SWARM_SRC})
target_link_libraries(tst_performance PRIVATE Qt6::Core Qt6::Positioning Qt6::Test)
add_test(NAME performance COMMAND tst_performance)
