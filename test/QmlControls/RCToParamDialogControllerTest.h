#pragma once

#include "VehicleTest.h"

class RCToParamDialogControllerTest : public VehicleTest
{
    Q_OBJECT
private slots:
    void _rejectUnboundFact();
    void _invalidateOnVehicleChange();
};
