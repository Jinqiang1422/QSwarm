# ============================================================================
# QML UI Smoke Tests
# ============================================================================

target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/QmlUITestBase.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/QmlUITestBase.h
        ${CMAKE_CURRENT_SOURCE_DIR}/VehicleConfigUITestBase.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/VehicleConfigUITestBase.h
        ${CMAKE_CURRENT_SOURCE_DIR}/TopLevelViewsTest.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/TopLevelViewsTest.h
        ${CMAKE_CURRENT_SOURCE_DIR}/PX4VehicleConfigUITest.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/PX4VehicleConfigUITest.h
        ${CMAKE_CURRENT_SOURCE_DIR}/APMVehicleConfigUITest.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/APMVehicleConfigUITest.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ToolbarIndicatorUITest.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/ToolbarIndicatorUITest.h
        ${CMAKE_CURRENT_SOURCE_DIR}/PlanViewUITest.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/PlanViewUITest.h
        ${CMAKE_CURRENT_SOURCE_DIR}/PlanViewLayerUITest.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/PlanViewLayerUITest.h
        ${CMAKE_CURRENT_SOURCE_DIR}/PX4SensorsCalibrationUITest.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/PX4SensorsCalibrationUITest.h
        ${CMAKE_CURRENT_SOURCE_DIR}/APMSensorsCalibrationUITest.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/APMSensorsCalibrationUITest.h
        ${CMAKE_CURRENT_SOURCE_DIR}/PX4AirframeSetupUITest.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/PX4AirframeSetupUITest.h
        ${CMAKE_CURRENT_SOURCE_DIR}/AppCloseWarningUITest.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/AppCloseWarningUITest.h
        ${CMAKE_CURRENT_SOURCE_DIR}/FlyViewProximityRadarUITest.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/FlyViewProximityRadarUITest.h
        ${CMAKE_CURRENT_SOURCE_DIR}/SwarmSituationLayerUITest.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/SwarmSituationLayerUITest.h
        ${CMAKE_CURRENT_SOURCE_DIR}/SwarmTaskEditorUITest.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/SwarmTaskEditorUITest.h
        ${CMAKE_CURRENT_SOURCE_DIR}/NTRIPSettingsUITest.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/NTRIPSettingsUITest.h
        ${CMAKE_CURRENT_SOURCE_DIR}/RemoteIDSettingsUITest.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/RemoteIDSettingsUITest.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_qgc_test(TopLevelViewsTest LABELS Integration NoSanitizer TIMEOUT 120)
add_qgc_test(PX4VehicleConfigUITest LABELS Integration NoSanitizer TIMEOUT 180)
add_qgc_test(APMVehicleConfigUITest LABELS Integration NoSanitizer TIMEOUT 360)
add_qgc_test(ToolbarIndicatorUITest LABELS Integration NoSanitizer TIMEOUT 180)
add_qgc_test(PlanViewUITest LABELS Integration NoSanitizer TIMEOUT 120)
add_qgc_test(PlanViewLayerUITest LABELS Integration NoSanitizer TIMEOUT 120)
add_qgc_test(PX4SensorsCalibrationUITest LABELS Integration NoSanitizer TIMEOUT 180)
add_qgc_test(APMSensorsCalibrationUITest LABELS Integration NoSanitizer TIMEOUT 240)
add_qgc_test(PX4AirframeSetupUITest LABELS Integration NoSanitizer TIMEOUT 120)
add_qgc_test(AppCloseWarningUITest LABELS Integration NoSanitizer TIMEOUT 240)
add_qgc_test(FlyViewProximityRadarUITest LABELS Integration NoSanitizer TIMEOUT 120)
# SwarmLocked marks the tests that are meaningful in a delivery build (autodispatch off).
# These two carry it because that build hides the autonomy controls, which changes the
# panel layout -- a layout that had never been exercised until it was added here.
set(SWARM_UI_LOCKED_LABEL "")
if(NOT QGC_SWARM_AI_AUTODISPATCH)
    set(SWARM_UI_LOCKED_LABEL SwarmLocked)
endif()
add_qgc_test(SwarmSituationLayerUITest LABELS Integration NoSanitizer ${SWARM_UI_LOCKED_LABEL} TIMEOUT 120)
add_qgc_test(SwarmTaskEditorUITest LABELS Integration NoSanitizer ${SWARM_UI_LOCKED_LABEL} TIMEOUT 120)
add_qgc_test(NTRIPSettingsUITest LABELS Integration NoSanitizer TIMEOUT 120)
add_qgc_test(RemoteIDSettingsUITest LABELS Integration NoSanitizer TIMEOUT 120)
