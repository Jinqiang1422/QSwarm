#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <memory>

#include "BaseClasses/VehicleTest.h"

class SwarmManager;

class SwarmExecutionTest : public VehicleTest
{
    Q_OBJECT
public:
    using VehicleTest::VehicleTest;
    ~SwarmExecutionTest() override;

private slots:
    void init() override;
    void cleanup() override;
    void _aiConfirmationCannotBeDisabled_data();
    void _aiConfirmationCannotBeDisabled();
    void _constraintErrorsSurviveConfirmationAndEditing();
    void _invalidPlansAreRejectedAtomically_data();
    void _invalidPlansAreRejectedAtomically();
    void _boundTargetsSurviveSelectionChanges();
    void _blockedStageCannotAdvance_data();
    void _blockedStageCannotAdvance();
    void _coverageCoordinatesReachTheChecker();
    void _candidateCoverageKeepsCoordinates();
    void _credentialsAreSessionOnly();

private:
    void _requestPlan(const QJsonArray& actions, bool functionCalling = false);
    void _writeConfig(const QJsonObject& config);
    std::unique_ptr<SwarmManager> _manager;
    QPointer<MockLink> _secondaryLink;
    QString _configPath;
    QByteArray _previousConfig;
    bool _hadConfig = false;
};
