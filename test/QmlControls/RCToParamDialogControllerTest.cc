#include "RCToParamDialogControllerTest.h"

#include "Fact.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "RCToParamDialogController.h"
#include "Vehicle.h"

void RCToParamDialogControllerTest::_rejectUnboundFact()
{
    RCToParamDialogController controller;
    Fact unrelated(1, QStringLiteral("TEST_PARAM"), FactMetaData::valueTypeFloat);
    controller.bind(nullptr, &unrelated);
    QVERIFY(!controller.tuningFact());
    controller.setTuningFact(&unrelated);
    QVERIFY(!controller.tuningFact());
    QVERIFY(!controller.apply("1", "0", 0, "-1", "1"));
    controller.setTuningFact(nullptr);
    QVERIFY(!controller.tuningFact());
}

void RCToParamDialogControllerTest::_invalidateOnVehicleChange()
{
    QVERIFY(vehicle());
    auto* parameters = vehicle()->parameterManager();
    QVERIFY(parameters);
    const auto names = parameters->parameterNames(ParameterManager::defaultComponentId);
    QVERIFY(!names.isEmpty());
    Fact* fact = parameters->getParameter(ParameterManager::defaultComponentId, names.first());
    QVERIFY(fact);
    RCToParamDialogController controller;
    controller.bind(vehicle(), fact);
    QCOMPARE(controller.tuningFact(), fact);
    QTRY_VERIFY_WITH_TIMEOUT(controller.property("ready").toBool(), TestTimeout::longMs());
    QVERIFY(!controller.apply("abc", "0", 0, "-1", "1"));
    MultiVehicleManager::instance()->setActiveVehicle(nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.property("ready").toBool(), TestTimeout::longMs());
    QVERIFY(!controller.tuningFact());
    QVERIFY(!controller.apply("1", "0", 0, "-1", "1"));
    MultiVehicleManager::instance()->setActiveVehicle(vehicle());
    QTRY_COMPARE_WITH_TIMEOUT(MultiVehicleManager::instance()->activeVehicle(), vehicle(), TestTimeout::longMs());
    QVERIFY(!controller.property("ready").toBool());
}

UT_REGISTER_TEST(RCToParamDialogControllerTest, TestLabel::Integration, TestLabel::Vehicle)
