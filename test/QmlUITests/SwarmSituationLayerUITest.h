#pragma once

#include "QmlUITestBase.h"

class SwarmSituationLayerUITest : public QmlUITestBase
{
    Q_OBJECT

private slots:
    void cleanup() override;
    void _testDelegatesFollowCOPModel();
};
