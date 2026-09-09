#pragma once

#include "UnitTest.h"

class QGCImageProviderTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _optionalSize();
    void _invalidId_data();
    void _invalidId();
    void _vehicleImages();
};
