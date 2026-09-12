#include "SwarmTaskEditTest.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>

#include "SwarmAIAgent.h"
#include "SwarmManager.h"
#include "SwarmTaskController.h"
#include "Vehicle.h"

SwarmTaskEditTest::~SwarmTaskEditTest() = default;

void SwarmTaskEditTest::init()
{
    VehicleTest::init();
    QVERIFY(vehicle());
    ignoreLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression("(集群任务|阶段 |阶段「|已生成多阶段任务计划|[0-9]+ 号机：)"));
    _manager = std::make_unique<SwarmManager>();
    _manager->setAutonomyLevel(2);
}

void SwarmTaskEditTest::cleanup()
{
    if (_manager)
        _manager->shutdown();
    _manager.reset();
    VehicleTest::cleanup();
}

// 通过 AI 代理的 JSON 入口挂起一个两阶段任务，等价于模型返回 start_swarm_task。
void SwarmTaskEditTest::_stagePendingTask()
{
    const QString json = QStringLiteral(R"([{
        "type": "start_swarm_task",
        "task_name": "编辑用例",
        "stages": [
            {"name": "起飞", "actions": [{"type": "takeoff", "altitude": 30}],
             "complete_when": {"kind": "all_airborne", "min_altitude": 25, "timeout_sec": 600}},
            {"name": "等待", "actions": [],
             "complete_when": {"kind": "duration", "seconds": 10, "timeout_sec": 600}}
        ]}])");
    const QJsonArray actions = QJsonDocument::fromJson(json.toUtf8()).array();
    _manager->aiAgent()->handleActionsJsonForTest(actions);
    QVERIFY(_manager->aiAgent()->planPending());
}

void SwarmTaskEditTest::pendingTaskModelMustExposeStagesActionsAndCondition()
{
    _stagePendingTask();
    const QVariantList model = _manager->aiAgent()->pendingTaskModel();
    QCOMPARE(model.count(), 2);

    const QVariantMap stage0 = model.at(0).toMap();
    QCOMPARE(stage0.value("name").toString(), QStringLiteral("起飞"));
    QCOMPARE(stage0.value("conditionKind").toString(), QStringLiteral("all_airborne"));
    const QJsonObject cond0 = QJsonDocument::fromJson(stage0.value("conditionParamsJson").toString().toUtf8()).object();
    QCOMPARE(cond0.value("timeout_sec").toInt(), 600);

    const QVariantList actions0 = stage0.value("actions").toList();
    QCOMPARE(actions0.count(), 1);
    const QVariantMap action0 = actions0.at(0).toMap();
    QCOMPARE(action0.value("type").toString(), QStringLiteral("takeoff"));
    QVERIFY(action0.value("editable").toBool());
    const QJsonObject params0 = QJsonDocument::fromJson(action0.value("paramsJson").toString().toUtf8()).object();
    QCOMPARE(params0.value("altitude").toDouble(), 30.0);

    QCOMPARE(model.at(1).toMap().value("conditionKind").toString(), QStringLiteral("duration"));
}

UT_REGISTER_TEST(SwarmTaskEditTest, TestLabel::Integration, TestLabel::Vehicle)
