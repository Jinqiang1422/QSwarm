target_sources(${CMAKE_PROJECT_NAME} PRIVATE SwarmExecutionTest.cc SwarmExecutionTest.h)
add_qgc_test(SwarmExecutionTest LABELS Integration Vehicle)
