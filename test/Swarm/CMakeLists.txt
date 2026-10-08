target_sources(${CMAKE_PROJECT_NAME} PRIVATE FormationLegPlannerTest.cc FormationLegPlannerTest.h)
add_qgc_test(FormationLegPlannerTest LABELS Unit)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE SwarmExecutionTest.cc SwarmExecutionTest.h)
add_qgc_test(SwarmExecutionTest LABELS Integration Vehicle TIMEOUT ${QGC_TEST_TIMEOUT_EXTENDED})

target_sources(${CMAKE_PROJECT_NAME} PRIVATE SwarmVehicleTest.cc SwarmVehicleTest.h)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE SwarmRegressionTest.cc SwarmRegressionTest.h)
add_qgc_test(SwarmRegressionTest LABELS Integration Vehicle TIMEOUT ${QGC_TEST_TIMEOUT_EXTENDED})

target_sources(${CMAKE_PROJECT_NAME} PRIVATE SwarmFormationTest.cc SwarmFormationTest.h)
add_qgc_test(SwarmFormationTest LABELS Integration Vehicle TIMEOUT ${QGC_TEST_TIMEOUT_EXTENDED})

target_sources(${CMAKE_PROJECT_NAME} PRIVATE SwarmSafetyTest.cc SwarmSafetyTest.h)
add_qgc_test(SwarmSafetyTest LABELS Integration Vehicle)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE SwarmTaskEditTest.cc SwarmTaskEditTest.h)
add_qgc_test(SwarmTaskEditTest LABELS Integration Vehicle)

if(NOT QGC_SWARM_AI_AUTODISPATCH)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE SwarmDispatchLockTest.cc SwarmDispatchLockTest.h)
    add_qgc_test(SwarmDispatchLockTest LABELS Integration Vehicle SwarmLocked)
endif()
