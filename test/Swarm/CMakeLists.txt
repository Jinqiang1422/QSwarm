target_sources(${CMAKE_PROJECT_NAME} PRIVATE SwarmExecutionTest.cc SwarmExecutionTest.h)
add_qgc_test(SwarmExecutionTest LABELS Integration Vehicle)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE SwarmRegressionTest.cc SwarmRegressionTest.h)
add_qgc_test(SwarmRegressionTest LABELS Integration Vehicle)
