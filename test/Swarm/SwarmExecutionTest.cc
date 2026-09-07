#include "SwarmExecutionTest.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "SwarmAIAgent.h"
#include "SwarmManager.h"
#include "SwarmMissionPlanner.h"
#include "SwarmTaskController.h"
#include "Vehicle.h"

SwarmExecutionTest::~SwarmExecutionTest() = default;

void SwarmExecutionTest::init()
{
    VehicleTest::init();
    QVERIFY(vehicle());
    // Safety rejections and task progress are expected UI notifications; retain strict logging elsewhere.
    ignoreLogMessage(
        "API.QGCApplication.AppMessage", QtDebugMsg,
        QRegularExpression("(AI 已生成|约束校验未通过|计划未下发|编辑未生效|集群任务|阶段 |检测到重启前未完成任务)"));
    _configPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/SwarmConfig/fences.json";
    QFile file(_configPath);
    _hadConfig = file.exists();
    _previousConfig = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    _writeConfig({});
    QFile::remove(QFileInfo(_configPath).absolutePath() + "/tasks.json");
    _manager = std::make_unique<SwarmManager>();
    _manager->setAutonomyLevel(2);
    mockLink()->clearReceivedMavCommandCounts();
}

void SwarmExecutionTest::cleanup()
{
    if (_manager)
        _manager->shutdown();
    _manager.reset();
    QFile::remove(QFileInfo(_configPath).absolutePath() + "/tasks.json");
    if (_secondaryLink) {
        MultiVehicleManager::instance()->setActiveVehicle(vehicle());
        _secondaryLink->disconnect();
        _secondaryLink = nullptr;
        QTRY_COMPARE_WITH_TIMEOUT(MultiVehicleManager::instance()->vehicles()->count(), 1, TestTimeout::longMs());
    }
    QFile file(_configPath);
    if (_hadConfig) {
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(_previousConfig), _previousConfig.size());
    } else {
        file.remove();
    }
    MultiVehicleManager::instance()->deselectAllVehicles();
    VehicleTest::cleanup();
}

void SwarmExecutionTest::_writeConfig(const QJsonObject& config)
{
    QDir().mkpath(QFileInfo(_configPath).absolutePath());
    QFile file(_configPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const auto bytes = QJsonDocument(config).toJson();
    QCOMPARE(file.write(bytes), bytes.size());
}

void SwarmExecutionTest::_requestPlan(const QJsonArray& actions, bool functionCalling)
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QString content =
        QString::fromUtf8(QJsonDocument(QJsonObject{{"reply", "test plan"}, {"actions", actions}}).toJson());
    QJsonObject message{{"role", "assistant"}, {"content", content}};
    if (functionCalling) {
        QJsonArray calls;
        for (const auto& value : actions) {
            QJsonObject arguments = value.toObject();
            const QString name = arguments.take("type").toString();
            calls.append(
                QJsonObject{{"id", QStringLiteral("call_%1").arg(calls.size())},
                            {"type", "function"},
                            {"function", QJsonObject{{"name", name},
                                                     {"arguments", QString::fromUtf8(QJsonDocument(arguments).toJson(
                                                                       QJsonDocument::Compact))}}}});
        }
        message["content"] = "test plan";
        message["tool_calls"] = calls;
    }
    const QByteArray body =
        QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{{"message", message}}}}}).toJson();
    connect(&server, &QTcpServer::newConnection, &server, [&server, body] {
        auto* socket = server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, socket, [socket, body, buffer = QByteArray()]() mutable {
            buffer += socket->readAll();
            const int end = buffer.indexOf("\r\n\r\n");
            if (end < 0)
                return;
            int length = 0;
            for (const auto& line : buffer.left(end).split('\n')) {
                if (line.toLower().startsWith("content-length:"))
                    length = line.mid(15).trimmed().toInt();
            }
            if (buffer.size() < end + 4 + length)
                return;
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                          QByteArray::number(body.size()) + "\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });
    auto* ai = _manager->aiAgent();
    ai->setApiBaseUrl(QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort()));
    ai->setApiKey(QStringLiteral("local-test-session-key"));
    ai->setUseFunctionCalling(functionCalling);
    ai->setRequireConfirm(false);
    ai->sendText(QStringLiteral("测试计划"), vehicle()->coordinate().latitude(), vehicle()->coordinate().longitude());
    QTRY_VERIFY_WITH_TIMEOUT(!ai->busy(), TestTimeout::longMs());
}

void SwarmExecutionTest::_aiConfirmationCannotBeDisabled_data()
{
    QTest::addColumn<int>("level");
    QTest::addColumn<QString>("type");
    QTest::addColumn<bool>("functionCalling");
    QTest::newRow("L2-arm") << 2 << QStringLiteral("arm") << false;
    QTest::newRow("L3-outside-allowlist") << 3 << QStringLiteral("arm") << false;
    QTest::newRow("L3-emergency") << 3 << QStringLiteral("emergency_disarm") << false;
    QTest::newRow("FC-L2-arm") << 2 << QStringLiteral("arm") << true;
}

void SwarmExecutionTest::_aiConfirmationCannotBeDisabled()
{
    QFETCH(int, level);
    QFETCH(QString, type);
    QFETCH(bool, functionCalling);
    _manager->setAutonomyLevel(level);
    _manager->setL3AllowedTypes({});
    QJsonObject action{{"type", type}};
    if (type == "arm")
        action["arm"] = true;
    _requestPlan({action}, functionCalling);
    QVERIFY(_manager->aiAgent()->planPending());
    QCOMPARE(mockLink()->receivedMavCommandCount(MAV_CMD_COMPONENT_ARM_DISARM), 0);
    QStringList errors;
    const auto plan = _manager->parseAndValidate({action}, vehicle()->coordinate(), errors);
    QVERIFY(!plan.isEmpty());
    QVERIFY(!_manager->executeActions(plan, QStringLiteral("ai_confirmed")));  // origin alone is not approval
    _manager->aiAgent()->confirmPlan();
    QTRY_COMPARE_WITH_TIMEOUT(mockLink()->receivedMavCommandCount(MAV_CMD_COMPONENT_ARM_DISARM), 1,
                              TestTimeout::longMs());
    QVERIFY(!_manager->aiAgent()->planPending());
}

void SwarmExecutionTest::_constraintErrorsSurviveConfirmationAndEditing()
{
    _writeConfig({{"max_altitude_m", 20.0}});
    _requestPlan({QJsonObject{{"type", "takeoff"}, {"altitude", 30.0}}});
    auto* ai = _manager->aiAgent();
    QVERIFY(ai->planPending());
    ai->confirmPlan();
    QVERIFY(ai->planPending());
    QCOMPARE(mockLink()->receivedMavCommandCount(MAV_CMD_COMPONENT_ARM_DISARM), 0);
    QCOMPARE(mockLink()->receivedMavCommandCount(MAV_CMD_NAV_TAKEOFF), 0);
    ai->updatePendingAction(0, QStringLiteral("{\"altitude\":10}"));
    const auto safe = ai->pendingPlanParamsJson();
    ai->updatePendingAction(0, QStringLiteral("{\"altitude\":40}"));
    QCOMPARE(ai->pendingPlanParamsJson(), safe);
    _writeConfig({{"max_altitude_m", 5.0}});  // telemetry/config can change after editing
    ai->confirmPlan();
    QVERIFY(ai->planPending());
    QCOMPARE(mockLink()->receivedMavCommandCount(MAV_CMD_NAV_TAKEOFF), 0);
}

void SwarmExecutionTest::_invalidPlansAreRejectedAtomically_data()
{
    QTest::addColumn<QJsonObject>("bad");
    QTest::newRow("unknown-target") << QJsonObject{{"type", "set_selection"}, {"vehicle_ids", QJsonArray{255}}};
    QTest::newRow("negative-altitude") << QJsonObject{{"type", "takeoff"}, {"altitude", -5}};
    QTest::newRow("string-altitude") << QJsonObject{{"type", "takeoff"}, {"altitude", "30"}};
    QTest::newRow("missing-altitude") << QJsonObject{{"type", "takeoff"}};
    QTest::newRow("unknown-field") << QJsonObject{{"type", "arm"}, {"arm", true}, {"unexpected", true}};
}

void SwarmExecutionTest::_invalidPlansAreRejectedAtomically()
{
    QFETCH(QJsonObject, bad);
    QStringList errors;
    const auto plan =
        _manager->parseAndValidate({bad, QJsonObject{{"type", "arm"}, {"arm", true}}}, vehicle()->coordinate(), errors);
    QVERIFY(plan.isEmpty());
    QVERIFY(!errors.isEmpty());
    QCOMPARE(mockLink()->receivedMavCommandCount(MAV_CMD_COMPONENT_ARM_DISARM), 0);
}

void SwarmExecutionTest::_boundTargetsSurviveSelectionChanges()
{
    auto* mvm = MultiVehicleManager::instance();
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg, QRegularExpression("Connected to Vehicle [0-9]+"));
    _secondaryLink = MockLink::startPX4MockLink();
    QVERIFY(_secondaryLink);
    QTRY_COMPARE_WITH_TIMEOUT(mvm->vehicles()->count(), 2, TestTimeout::longMs());
    Vehicle* secondary = nullptr;
    for (int i = 0; i < mvm->vehicles()->count(); ++i) {
        auto* candidate = qobject_cast<Vehicle*>(mvm->vehicles()->get(i));
        if (candidate && candidate != vehicle())
            secondary = candidate;
    }
    QVERIFY(secondary);
    QTRY_VERIFY_WITH_TIMEOUT(secondary->isInitialConnectComplete(), TestTimeout::longMs());
    verifyExpectedLogMessage();
    _secondaryLink->clearReceivedMavCommandCounts();
    QStringList errors;
    mvm->selectVehicle(vehicle()->id());
    auto plan =
        _manager->parseAndValidate({QJsonObject{{"type", "arm"}, {"arm", true}}}, vehicle()->coordinate(), errors);
    QVERIFY(!plan.isEmpty());
    QCOMPARE(plan.first().targetIds, QList<int>{vehicle()->id()});
    const auto coverage = _manager->parseAndValidate(
        {QJsonObject{{"type", "plan_coverage"}, {"length", 30}, {"width", 30}}}, vehicle()->coordinate(), errors);
    QCOMPARE(coverage.size(), 1);
    mvm->deselectAllVehicles();
    mvm->selectVehicle(secondary->id());
    QVERIFY(_manager->executeActions(plan, "test", SwarmManager::ExecutionApproval::OperatorConfirmed));
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->armed(), TestTimeout::longMs());
    QCOMPARE(_secondaryLink->receivedMavCommandCount(MAV_CMD_COMPONENT_ARM_DISARM), 0);
    QVERIFY(!secondary->armed());
    QSignalSpy uploaded(_manager->missionPlanner(), &SwarmMissionPlanner::missionUploaded);
    QVERIFY(uploaded.isValid());
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression("区域覆盖任务已提交上传请求：1 架无人机"));
    QVERIFY(_manager->executeActions(coverage, "test", SwarmManager::ExecutionApproval::OperatorConfirmed));
    QTRY_COMPARE_WITH_TIMEOUT(uploaded.count(), 1, TestTimeout::longMs());
    verifyExpectedLogMessage();
    QCOMPARE(uploaded.first().first().toInt(), vehicle()->id());
    QCOMPARE(_manager->missionPlanner()->lastPlanVehicleCount(), 1);
    plan.first().targetIds = {255};
    QVERIFY(!_manager->executeActions(plan, "test", SwarmManager::ExecutionApproval::OperatorConfirmed));
    QCOMPARE(mockLink()->receivedMavCommandCount(MAV_CMD_COMPONENT_ARM_DISARM), 1);
}

void SwarmExecutionTest::_blockedStageCannotAdvance_data()
{
    QTest::addColumn<QString>("condition");
    QTest::newRow("duration") << QStringLiteral("duration");
    QTest::newRow("manual") << QStringLiteral("manual");
}

void SwarmExecutionTest::_blockedStageCannotAdvance()
{
    QFETCH(QString, condition);
    _writeConfig({{"max_altitude_m", 20.0}});
    QStringList errors;
    const auto actions = _manager->parseAndValidate({QJsonObject{{"type", "takeoff"}, {"altitude", 30.0}}},
                                                    vehicle()->coordinate(), errors);
    SwarmTaskController::Stage stage{"blocked", actions, condition, {{"seconds", 0}, {"grace_sec", 0}}};
    auto* task = _manager->taskController();
    QVERIFY(task->startTask("blocked test", {stage, {"next", {}, "manual", {}}}, true));
    QVERIFY(task->blocked());
    QVERIFY(!task->waitingForProceed());
    QVERIFY(QMetaObject::invokeMethod(task, "_tick", Qt::DirectConnection));
    task->proceed();
    task->retryBlockedStage();
    QCOMPARE(task->currentStageIndex(), 0);
    QVERIFY(task->blocked());
    QCOMPARE(mockLink()->receivedMavCommandCount(MAV_CMD_COMPONENT_ARM_DISARM), 0);
    QCOMPARE(mockLink()->receivedMavCommandCount(MAV_CMD_NAV_TAKEOFF), 0);
    _writeConfig({{"max_altitude_m", 40.0}});
    task->retryBlockedStage();
    QVERIFY(!task->blocked());
    QCOMPARE(task->currentStageIndex(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(mockLink()->receivedMavCommandCount(MAV_CMD_COMPONENT_ARM_DISARM), 1,
                              TestTimeout::longMs());
}

void SwarmExecutionTest::_coverageCoordinatesReachTheChecker()
{
    const QGeoCoordinate center = vehicle()->coordinate().atDistanceAndAzimuth(1000, 0);
    const double lat = center.latitude(), lon = center.longitude();
    const QJsonArray polygon{QJsonArray{lat - .001, lon - .001}, QJsonArray{lat - .001, lon + .001},
                             QJsonArray{lat + .001, lon + .001}, QJsonArray{lat + .001, lon - .001}};
    _writeConfig({{"exclude", QJsonArray{QJsonObject{{"polygon", polygon}}}}});
    QStringList errors;
    const auto actions = _manager->parseAndValidate(
        {QJsonObject{{"type", "plan_coverage"}, {"center", QJsonArray{lat, lon}}, {"length", 20}, {"width", 20}}},
        vehicle()->coordinate(), errors);
    QCOMPARE(actions.size(), 1);
    QCOMPARE(actions.first().params.value("center_lat").toDouble(), lat);
    QVERIFY(_manager->planHasConstraintError(actions));
    QVERIFY(!_manager->executeActions(actions, "test", SwarmManager::ExecutionApproval::OperatorConfirmed));
}

void SwarmExecutionTest::_credentialsAreSessionOnly()
{
    auto* ai = _manager->aiAgent();
    ai->setApiKey("session-only-marker");
    QCOMPARE(ai->apiKey(), QStringLiteral("session-only-marker"));
    QVERIFY(!QSettings().contains("SwarmAI/apiKey"));
}

void SwarmExecutionTest::_candidateCoverageKeepsCoordinates()
{
    const auto center = vehicle()->coordinate();
    const QJsonObject coverage{{"type", "plan_coverage"},
                               {"length", 30},
                               {"width", 30},
                               {"center_lat", center.latitude()},
                               {"center_lon", center.longitude()}};
    const QJsonArray candidates{
        QJsonObject{{"title", "invalid"}, {"actions", QJsonArray{QJsonObject{{"type", "takeoff"}, {"altitude", -1}}}}},
        QJsonObject{{"title", "valid"}, {"actions", QJsonArray{coverage}}}};
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg, QRegularExpression("AI 给出 1 个候选方案"));
    _requestPlan({QJsonObject{{"type", "propose_candidates"}, {"candidates", candidates}}}, true);
    verifyExpectedLogMessage();
    auto* ai = _manager->aiAgent();
    QCOMPARE(ai->candidates().size(), 1);
    QCOMPARE(ai->candidates().first().toMap().value("index").toInt(), 0);
    ai->setRequireConfirm(true);
    ai->selectCandidate(0);
    QVERIFY(ai->planPending());
    const auto params = QJsonDocument::fromJson(ai->pendingPlanParamsJson().first().toUtf8()).object();
    QCOMPARE(params.value("center_lat").toDouble(), center.latitude());
    QStringList errors;
    QJsonObject mapCoverage{{"type", "plan_coverage"}, {"length", 30}, {"width", 30}, {"center", "map_center"}};
    const auto plan = _manager->parseAndValidate({mapCoverage}, center, errors);
    QCOMPARE(plan.size(), 1);
    QCOMPARE(plan.first().params.value("center_lat").toDouble(), center.latitude());
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg, QRegularExpression("已放弃 AI 行动计划"));
    _requestPlan({QJsonObject{{"type", "takeoff"}, {"altitude", -1}}});
    verifyExpectedLogMessage();
    QVERIFY(!ai->planPending());
    QVERIFY(ai->candidates().isEmpty());
}

UT_REGISTER_TEST(SwarmExecutionTest, TestLabel::Integration, TestLabel::Vehicle)
