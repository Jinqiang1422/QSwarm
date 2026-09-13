#include "SwarmTaskEditorUITest.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QTest>

#include "MockLink.h"
#include "SwarmAIAgent.h"
#include "SwarmManager.h"
#include "Vehicle.h"

namespace {

/// Builds a single-stage, single-action start_swarm_task JSON, matching the
/// shape SwarmAIAgent::handleActionsJsonForTest() expects (see
/// SwarmTaskEditTest::_stagePendingTask() for the non-UI equivalent). Kept to
/// one stage/one action so findVisibleItem's first-match lookup of the
/// swarmStageAction*/swarmStageCondition* objectNames is unambiguous -- those
/// objectNames live inside nested Repeater delegates and would otherwise
/// repeat once per stage/action.
QJsonArray oneStageTaskJson(const QString& actionJson, const QString& conditionJson)
{
    const QString json = QStringLiteral(R"([{
        "type": "start_swarm_task",
        "task_name": "UI 编辑用例",
        "stages": [{"name": "阶段一", "actions": [%1], "complete_when": %2}]
    }])")
                             .arg(actionJson, conditionJson);
    return QJsonDocument::fromJson(json.toUtf8()).array();
}

QJsonArray takeoffStageJson()
{
    return oneStageTaskJson(QStringLiteral(R"({"type": "takeoff", "altitude": 30})"),
                            QStringLiteral(R"({"kind": "all_airborne", "min_altitude": 25, "timeout_sec": 600})"));
}

// vehicleId must be the MockLink vehicle's real system id (assigned from 128 upward),
// never a hardcoded value -- see SwarmTaskEditTest::editingSetSelectionMustBeRejected()
// for why a fixed id would let the "not online" guard mask the one this test targets.
QJsonArray setSelectionStageJson(int vehicleId)
{
    return oneStageTaskJson(QStringLiteral(R"({"type": "set_selection", "vehicle_ids": [%1]})").arg(vehicleId),
                            QStringLiteral(R"({"kind": "manual", "timeout_sec": 600})"));
}

}  // namespace

UT_REGISTER_TEST(SwarmTaskEditorUITest, TestLabel::Integration)

void SwarmTaskEditorUITest::init()
{
    UnitTest::init();

    // Staging a pending task via handleActionsJsonForTest() and later discarding it in
    // cleanup() both route through SwarmAIAgent::agentFeedback -> QGCApplication::
    // showAppMessage(), which strict log mode requires every test to account for. These
    // two messages are pure setup/teardown noise for this suite -- not the behavior any
    // of its tests are verifying -- so ignoreLogMessage() rather than
    // expectLogMessage()/verifyExpectedLogMessage() is the right tool here.
    ignoreLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("已生成多阶段任务计划")));
    ignoreLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("已放弃 AI 行动计划")));
}

void SwarmTaskEditorUITest::cleanup()
{
    // SwarmManager/SwarmAIAgent are process-wide singletons (Q_APPLICATION_STATIC), so a
    // task staged by one test slot would otherwise leak into the next. rejectPlan() is a
    // no-op when nothing is pending.
    SwarmManager::instance()->aiAgent()->rejectPlan();
    QmlUITestBase::cleanup();
}

// The default offscreen QQuickWindow size (derived from Screen.pixelDensity, see
// MainWindowSavedState.qml) is too short once a MockLink vehicle makes both
// SwarmControlPanel and SwarmAIPanel visible and they stack in the left column:
// SwarmAIPanel's maxHeight (parent.height - toolStrip.height - swarmControlPanel.height
// - margins, set in FlyViewWidgetLayer.qml) goes negative, collapsing its QGCFlickable to
// zero/negative height so nothing inside it -- including editParamsToggle -- can be
// scrolled to a clickable position. Growing the window is a test-only viewport change
// (like a user maximizing their window); it does not touch the panel's fixed
// ScreenTools.defaultFontPixelWidth * 24 width that the assertions below check, and it
// does not touch any production visibility/layout condition.
void SwarmTaskEditorUITest::_growWindowForPanel()
{
    _window->resize(900, 1600);
    QVERIFY(QTest::qWaitFor(
        [this]() {
            QQuickItem* const flickable = findVisibleItem(_rootItem, QStringLiteral("swarmAIPanelFlickable"), 0);
            return flickable && flickable->height() > 0;
        },
        3000));
}

// Enabling "编辑参数" reveals previously height-0 rows, growing mainColumn's implicit
// height over *several* polish passes spaced across multiple event-loop iterations (one
// reveal was observed to land its height in two visibly separate steps). scrollIntoView()
// (used by clickButtonScrolled()) tracks a single item's own position but only requires two
// back-to-back polls to agree, so a caller that scrolls to a *different* item right after
// such a reveal can sample the layout between two growth steps and be told it "settled"
// prematurely. Requiring contentHeight to be unchanged for a minimum wall-clock span, not
// just two consecutive polls, avoids that race.
bool SwarmTaskEditorUITest::_waitForContentHeightSettled(const QString& flickableObjectName)
{
    QQuickItem* const flickable = findVisibleItem(_rootItem, flickableObjectName);
    if (!flickable) {
        return false;
    }
    double lastHeight = -1.0;
    QElapsedTimer stableFor;
    stableFor.start();
    return QTest::qWaitFor(
        [&]() {
            const double height = flickable->property("contentHeight").toDouble();
            if (!qFuzzyCompare(height + 1.0, lastHeight + 1.0)) {
                lastHeight = height;
                stableFor.restart();
                return false;
            }
            return (height > 0) && (stableFor.elapsed() >= 250);
        },
        3000);
}

// Assertion 1 (the one that would have caught Task 7's Critical bug): the stage action
// edit field must be visible and *usably wide*, not just non-null. A field squeezed by an
// oversized label still exists and passes a null-pointer check, so this needs a real
// geometric property.
//
// QGCTextField carries a hard width floor (its background Rectangle's implicitWidth is
// ScreenTools.implicitTextFieldWidth, ~10 font units) that Qt Quick Layouts never
// compresses an item below by default. That floor was measured to exceed the apply
// button's own width in *both* the fixed and the re-broken layout (field ~60px, button
// ~39px either way) -- so a plain "field wider than the button" comparison, tried first,
// does NOT discriminate; see the task report for the measurements that caught this.
// What *does* change is how much of the row the field gets to keep: with the label back
// in the row (the historical bug), it takes a fixed 18-unit share off the top, so the
// field's portion of that row's own width drops sharply (measured 60.7% -> 28.6%).
// Requiring the field to hold the majority of its row is therefore the discriminating
// property: it fails exactly when something else in the row is competing for space with
// it, which is the mechanism of the bug this test guards against.
void SwarmTaskEditorUITest::_testStageActionFieldIsVisibleAndWide()
{
    runWithMockLink(
        [] { return MockLink::startPX4MockLink(); },
        [this](QPointer<MockLink> /*mockLink*/, Vehicle* /*vehicle*/) {
            SwarmAIAgent* const agent = SwarmManager::instance()->aiAgent();
            agent->handleActionsJsonForTest(takeoffStageJson());
            QVERIFY(agent->planPending());
            _growWindowForPanel();

            QVERIFY2(
                clickButtonScrolled(QStringLiteral("swarmEditParamsToggle"), QStringLiteral("swarmAIPanelFlickable")),
                "Could not click the '编辑参数' toggle");

            QQuickItem* const field = findVisibleItem(_rootItem, QStringLiteral("swarmStageActionField"));
            QVERIFY2(field, "Stage action edit field not visible after enabling edit mode");
            QQuickItem* const apply = findVisibleItem(_rootItem, QStringLiteral("swarmStageActionApply"));
            QVERIFY2(apply, "Stage action apply button not visible after enabling edit mode");
            QQuickItem* const row = field->parentItem();
            QVERIFY2(row, "Stage action edit field has no parent row");

            QVERIFY2(field->width() > 0, "Stage action edit field has zero width");
            QVERIFY2(field->width() * 2 > row->width(),
                     qPrintable(QStringLiteral("Stage action edit field (%1px) does not hold the majority of its "
                                               "row (%2px total, shared with the apply button) -- something else "
                                               "in the row is squeezing it")
                                    .arg(field->width())
                                    .arg(row->width())));
        });
}

// Assertion 2: an edit made through the field + apply button actually reaches
// SwarmAIAgent::pendingTaskModel(), not just the field's own local text property.
//
// The apply button is clicked by invoking its clicked() signal directly rather than
// through clickButton()/QTest::mouseClick(). That is a deliberate workaround for a
// separate, pre-existing layout defect discovered while writing this test (out of scope
// to fix here -- this file may only add objectNames): the action/condition QGCTextField's
// placeholderText ("参数 JSON，如 ...") is long enough that the field's implicit width is
// driven by the *placeholder*, not by Layout.fillWidth, regardless of what the field
// actually contains. The per-action RowLayout (unlike its sibling QGCLabel, which
// explicitly sets Layout.fillWidth: true) has no such declaration, so it sizes to its
// unconstrained preferred width -- field + button + spacing -- which was measured at
// 157px against a 132px-wide available column. The apply button therefore renders
// entirely outside the QGCFlickable's clipped, clickable region: scrollIntoView()
// (used by clickButtonScrolled()) correctly refuses to ever report it reachable, and even
// a raw QTest::mouseClick() at its true scene position (still inside the window) is
// silently swallowed because Qt Quick does not deliver pointer events to a point outside
// a clip: true ancestor's bounds. This reproduces with the field's *original*,
// never-edited text too, so it is unrelated to the specific value this test sets -- see
// the task report for the full geometry trace. Invoking clicked() still exercises the
// exact onClicked handler (and therefore SwarmAIAgent::updatePendingStageAction()) that a
// real click would.
void SwarmTaskEditorUITest::_testEditRoundTripsThroughUI()
{
    runWithMockLink(
        [] { return MockLink::startPX4MockLink(); },
        [this](QPointer<MockLink> /*mockLink*/, Vehicle* /*vehicle*/) {
            SwarmAIAgent* const agent = SwarmManager::instance()->aiAgent();
            agent->handleActionsJsonForTest(takeoffStageJson());
            QVERIFY(agent->planPending());
            _growWindowForPanel();

            QVERIFY2(
                clickButtonScrolled(QStringLiteral("swarmEditParamsToggle"), QStringLiteral("swarmAIPanelFlickable")),
                "Could not click the '编辑参数' toggle");
            QVERIFY2(_waitForContentHeightSettled(QStringLiteral("swarmAIPanelFlickable")),
                     "Panel content height never settled after enabling edit mode");

            QQuickItem* const field = findVisibleItem(_rootItem, QStringLiteral("swarmStageActionField"));
            QVERIFY2(field, "Stage action edit field not visible after enabling edit mode");

            // The field's "text" property is bound to modelData.paramsJson (see
            // SwarmAIPanel.qml); imperatively assigning it here breaks that binding exactly
            // as a live QTest::keyClick would, but sidesteps unreliable ASCII->key mapping
            // for JSON punctuation ('{', '"', ':') in the offscreen test platform.
            const QString newParams = QStringLiteral(R"({"altitude": 45})");
            QVERIFY(field->setProperty("text", newParams));
            QCOMPARE(field->property("text").toString(), newParams);

            QQuickItem* const apply = findVisibleItem(_rootItem, QStringLiteral("swarmStageActionApply"));
            QVERIFY2(apply, "Stage action apply button not visible after enabling edit mode");
            QVERIFY2(apply->property("enabled").toBool(), "Apply button is disabled");
            QVERIFY2(QMetaObject::invokeMethod(apply, "clicked"),
                     "Could not invoke clicked() on the stage apply button");

            const QVariantList model = agent->pendingTaskModel();
            QVERIFY2(!model.isEmpty(), "Pending task model is empty after apply");
            const QVariantList actions0 = model.at(0).toMap().value(QStringLiteral("actions")).toList();
            QVERIFY2(!actions0.isEmpty(), "Stage 0 has no actions after apply");
            const QJsonObject params =
                QJsonDocument::fromJson(actions0.at(0).toMap().value(QStringLiteral("paramsJson")).toString().toUtf8())
                    .object();
            QCOMPARE(params.value(QStringLiteral("altitude")).toDouble(), 45.0);
        });
}

// Assertion 3: a set_selection action's field must not be enabled for editing --
// changing the target machine mid-task would break the target-consistency assumption the
// rest of the stage's actions were validated under (see SwarmAIAgent::
// updatePendingStageAction()'s explicit set_selection guard).
void SwarmTaskEditorUITest::_testSetSelectionActionFieldDisabled()
{
    runWithMockLink([] { return MockLink::startPX4MockLink(); },
                    [this](QPointer<MockLink> /*mockLink*/, Vehicle* vehicle) {
                        SwarmAIAgent* const agent = SwarmManager::instance()->aiAgent();
                        agent->handleActionsJsonForTest(setSelectionStageJson(vehicle->id()));
                        QVERIFY(agent->planPending());
                        QVERIFY2(agent->pendingTaskModel()
                                         .at(0)
                                         .toMap()
                                         .value(QStringLiteral("actions"))
                                         .toList()
                                         .at(0)
                                         .toMap()
                                         .value(QStringLiteral("editable"))
                                         .toBool() == false,
                                 "Fixture bug: set_selection action unexpectedly reported editable");
                        _growWindowForPanel();

                        QVERIFY2(clickButtonScrolled(QStringLiteral("swarmEditParamsToggle"),
                                                     QStringLiteral("swarmAIPanelFlickable")),
                                 "Could not click the '编辑参数' toggle");

                        QVERIFY(verifyEnabled(QStringLiteral("swarmStageActionField"), false,
                                              QStringLiteral("set_selection action field")));
                    });
}
