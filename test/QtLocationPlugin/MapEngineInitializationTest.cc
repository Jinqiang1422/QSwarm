#include "MapEngineInitializationTest.h"

#include "QGeoServiceProviderPluginQGC.h"
#include "QGeoTiledMappingManagerEngineQGC.h"

void MapEngineInitializationTest::_missingQmlEngine()
{
    QGeoServiceProviderFactoryQGC factory;
    QGeoServiceProvider::Error error = QGeoServiceProvider::NoError;
    QString message;
    expectLogMessage("QtLocationPlugin.QGeoServiceProviderFactoryQGC", QtWarningMsg,
                     QRegularExpression("Mapping requested without a QML engine"));
    QVERIFY(!factory.createMappingManagerEngine({}, &error, &message));
    verifyExpectedLogMessage();
    QCOMPARE(error, QGeoServiceProvider::LoaderError);
    QVERIFY(!message.isEmpty());
    expectLogMessage("QtLocationPlugin.QGeoServiceProviderFactoryQGC", QtWarningMsg,
                     QRegularExpression("Mapping requested without a QML engine"));
    QVERIFY(!factory.createMappingManagerEngine({}, nullptr, nullptr));
    verifyExpectedLogMessage();
}

void MapEngineInitializationTest::_destroyedQmlEngine()
{
    QGeoServiceProviderFactoryQGC factory;
    {
        QQmlEngine engine;
        factory.setQmlEngine(&engine);
    }
    QGeoServiceProvider::Error error = QGeoServiceProvider::NoError;
    expectLogMessage("QtLocationPlugin.QGeoServiceProviderFactoryQGC", QtWarningMsg,
                     QRegularExpression("Mapping requested without a QML engine"));
    QVERIFY(!factory.createMappingManagerEngine({}, &error, nullptr));
    verifyExpectedLogMessage();
    QCOMPARE(error, QGeoServiceProvider::LoaderError);
}

void MapEngineInitializationTest::_missingNetworkManager()
{
    QGeoServiceProvider::Error error = QGeoServiceProvider::NoError;
    QString message;
    QGeoTiledMappingManagerEngineQGC engine({}, &error, &message, nullptr);
    QCOMPARE(error, QGeoServiceProvider::LoaderError);
    QVERIFY(!message.isEmpty());
    QGeoTiledMappingManagerEngineQGC engineWithoutOutputs({}, nullptr, nullptr, nullptr);
}

UT_REGISTER_TEST(MapEngineInitializationTest, TestLabel::Unit)
