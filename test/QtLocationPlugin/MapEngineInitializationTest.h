#pragma once

#include "UnitTest.h"

class MapEngineInitializationTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _missingQmlEngine();
    void _destroyedQmlEngine();
    void _missingNetworkManager();
};
