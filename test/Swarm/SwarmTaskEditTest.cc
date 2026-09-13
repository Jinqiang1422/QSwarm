#include "SwarmTaskEditTest.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtTest/QSignalSpy>

#include "SwarmAIAgent.h"
#include "SwarmManager.h"
#include "SwarmTaskController.h"
#include "Vehicle.h"

SwarmTaskEditTest::~SwarmTaskEditTest() = default;

void SwarmTaskEditTest::init()
{
    VehicleTest::init();
    QVERIFY(vehicle());
    ignoreLogMessage(
        "API.QGCApplication.AppMessage", QtDebugMsg,
        QRegularExpression("(集群任务|阶段 |阶段「|已生成多阶段任务计划|[0-9]+ 号机：|编辑未生效|阶段序号无效|"
                           "动作序号无效|修改目标机)"));
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

void SwarmTaskEditTest::editingStageActionMustRevalidateAndApply()
{
    _stagePendingTask();
    SwarmAIAgent* agent = _manager->aiAgent();
    agent->updatePendingStageAction(0, 0, QStringLiteral(R"({"altitude": 45})"));

    const QVariantMap stage0 = agent->pendingTaskModel().at(0).toMap();
    const QVariantMap action0 = stage0.value("actions").toList().at(0).toMap();
    const QJsonObject params = QJsonDocument::fromJson(action0.value("paramsJson").toString().toUtf8()).object();
    QCOMPARE(params.value("altitude").toDouble(), 45.0);
}

void SwarmTaskEditTest::invalidStageActionEditMustLeaveTaskUnchanged()
{
    _stagePendingTask();
    SwarmAIAgent* agent = _manager->aiAgent();
    const QVariantList before = agent->pendingTaskModel();

    // 起飞高度 5 米低于该阶段 all_airborne 的 min_altitude 25，parseStages 必须整体拒绝。
    agent->updatePendingStageAction(0, 0, QStringLiteral(R"({"altitude": 5})"));

    QCOMPARE(agent->pendingTaskModel(), before);
}

void SwarmTaskEditTest::editingSetSelectionMustBeRejected()
{
    // vehicle_ids 必须是当前在线机号（MockLink 分配的系统 ID 并非固定为 1），否则
    // set_selection 会先因“机号不在线”被拒绝，测试就验证不到本用例要覆盖的编辑拒绝路径。
    const QString json = QStringLiteral(R"([{
        "type": "start_swarm_task", "task_name": "选机用例",
        "stages": [{"name": "选机并起飞",
                    "actions": [{"type": "set_selection", "vehicle_ids": [%1]},
                                {"type": "takeoff", "altitude": 30}],
                    "complete_when": {"kind": "all_airborne", "min_altitude": 25, "timeout_sec": 600}}]}])")
                             .arg(vehicle()->id());
    _manager->aiAgent()->handleActionsJsonForTest(QJsonDocument::fromJson(json.toUtf8()).array());
    QVERIFY(_manager->aiAgent()->planPending());

    SwarmAIAgent* agent = _manager->aiAgent();
    QVERIFY(
        !agent->pendingTaskModel().at(0).toMap().value("actions").toList().at(0).toMap().value("editable").toBool());

    // 用信号内容而非仅状态不变来断言：机号 1/2 本身也不在线，若把 set_selection 守卫
    // 删掉，编辑仍会被 parseAndValidate 的“机号不在线”挡住、状态照样不变——那样这条
    // 用例就测不出守卫本身有没有生效。必须核对具体的反馈文案才能把两者区分开。
    QSignalSpy feedbackSpy(agent, &SwarmAIAgent::agentFeedback);
    QVERIFY(feedbackSpy.isValid());
    const QVariantList before = agent->pendingTaskModel();
    agent->updatePendingStageAction(0, 0, QStringLiteral(R"({"vehicle_ids": [1, 2]})"));
    QCOMPARE(agent->pendingTaskModel(), before);
    QCOMPARE(feedbackSpy.count(), 1);
    QCOMPARE(feedbackSpy.takeFirst().at(0).toString(),
             QStringLiteral("修改目标机请重新生成计划，以保持后续动作的目标一致"));
}

void SwarmTaskEditTest::outOfRangeStageOrActionIndexMustBeRejected()
{
    _stagePendingTask();
    SwarmAIAgent* agent = _manager->aiAgent();
    const QVariantList before = agent->pendingTaskModel();

    agent->updatePendingStageAction(5, 0, QStringLiteral(R"({"altitude": 45})"));
    agent->updatePendingStageAction(0, 9, QStringLiteral(R"({"altitude": 45})"));
    agent->updatePendingStageAction(-1, 0, QStringLiteral(R"({"altitude": 45})"));

    QCOMPARE(agent->pendingTaskModel(), before);
}

UT_REGISTER_TEST(SwarmTaskEditTest, TestLabel::Integration, TestLabel::Vehicle)
