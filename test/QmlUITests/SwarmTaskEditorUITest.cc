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
// oversized sibling still exists and passes a null-pointer check, so this needs a real
// geometric property.
//
// The field, button, and label are now each on their own line (SwarmAIPanel.qml) -- no
// two width-hungry items share a row any more, which is *why* Task 7's bug (and the
// row-overflow bug this test's earlier drafts found on top of it: see the task report)
// cannot recur in this structure. A "does the field hold the majority of its row"
// assertion (this test's previous version) would now be checking a row that no longer
// exists -- trivially true, since the field is alone there. The property that still
// falls out of the same failure mode is: the field, having nothing to share space with,
// should occupy nearly all of the panel's usable column width (measured against
// swarmAIPanelFlickable's width, a stable reference independent of the field's own
// immediate container). Confirmed by the discrimination experiment (see the task report)
// that reintroducing a wide sibling into the field's line drops this ratio from ~96% to
// ~43%, comfortably crossing the 80% threshold used here.
//
// UPDATE (Task 7c): the ">0.8" ratio above has no upper bound by itself and would not have
// caught the *overflow* mechanism this test previously documented as an unfixed defect --
// the field's own implicit width could inflate past swarmAIPanelFlickable's clip width while
// still trivially satisfying ">80%". Task 7c found and fixed the actual root cause: two
// QGCLabels in this same shared planColumn (the flat-plan summary line and the stage
// condition-kind hint, both wrapMode: Text.Wrap with no width cap) reported their full
// *unwrapped* text as implicitWidth, which a single-column ColumnLayout used to override
// anchors.fill's ~132px available width and drag every sibling row -- including this field --
// out past the flickable's clip bounds. The fix (SwarmAIPanel.qml) gives every such QGCLabel
// and QGCTextField in the plan-confirm area a Layout.maximumWidth (measurement showed
// Layout.minimumWidth alone has no effect on this inflation) so no single row can ever force
// the column wider than the panel. The second assertion below turns that fix into a
// regression guard: the field must not just be *wide enough* (first assertion) but also never
// wider than the flickable that clips it -- exactly the property whose absence let Task 7's
// bug ship unnoticed. Verified to discriminate: temporarily removing the QGCTextField/QGCLabel
// Layout.maximumWidth overrides reproduces the original failure (field width 157px vs
// flickable width 138px) and fails this new assertion; restoring them passes it.
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
            QVERIFY2(_waitForContentHeightSettled(QStringLiteral("swarmAIPanelFlickable")),
                     "Panel content height never settled after enabling edit mode");

            QQuickItem* const field = findVisibleItem(_rootItem, QStringLiteral("swarmStageActionField"));
            QVERIFY2(field, "Stage action edit field not visible after enabling edit mode");
            QQuickItem* const apply = findVisibleItem(_rootItem, QStringLiteral("swarmStageActionApply"));
            QVERIFY2(apply, "Stage action apply button not visible after enabling edit mode");
            QQuickItem* const flickable = findVisibleItem(_rootItem, QStringLiteral("swarmAIPanelFlickable"));
            QVERIFY2(flickable, "SwarmAIPanel flickable not visible");

            QVERIFY2(field->width() > 0, "Stage action edit field has zero width");
            QVERIFY2(field->width() > 0.8 * flickable->width(),
                     qPrintable(QStringLiteral("Stage action edit field (%1px) does not occupy a large fraction "
                                               "of the panel's usable width (%2px) -- something is squeezing it")
                                    .arg(field->width())
                                    .arg(flickable->width())));
            // Upper bound: the field must never be wider than the flickable that clips it --
            // Task 7's original bug (field width 157px vs flickable width 138px) satisfied the
            // ">0.8" lower bound above while still overflowing the clip region and making the
            // apply button unreachable. See the block comment above this test for how this was
            // verified to discriminate.
            QVERIFY2(field->width() <= flickable->width(),
                     qPrintable(QStringLiteral("Stage action edit field (%1px) is wider than "
                                               "swarmAIPanelFlickable (%2px) -- it overflows the panel's "
                                               "clip region even though it passes the ratio check above")
                                    .arg(field->width())
                                    .arg(flickable->width())));
        });
}

// Assertion 2: an edit made through the field + apply button actually reaches
// SwarmAIAgent::pendingTaskModel(), not just the field's own local text property, via a
// genuine QTest::mouseClick() (through clickButtonScrolled()), not a signal invocation --
// a real click must land for this to prove an operator can actually reach and press the
// button, not merely that the handler code works.
//
// STATUS (Task 7c): FIXED. Two earlier fix attempts (recorded in git history) failed to
// make this button reachable: first, adding Layout.fillWidth: true to a shared RowLayout
// only changed what that row *asked its parent for*, not what it *needed*; second, moving
// the field and button to separate lines (still the structure today) removed row-sharing
// but the field's own implicit width (157px, against swarmAIPanelFlickable's 138px) still
// overflowed the panel on its own. Both attempts assumed the QGCTextField itself was the
// forcing element. Direct measurement (walking planColumn's children and printing
// implicitWidth/width for each -- see the task report) disproved that: the QGCTextFields
// were never the culprit (implicitWidth 60px, well under budget). The actual forcing
// elements were two QGCLabels sharing the same planColumn -- the flat-plan editor's summary
// line (SwarmAIPanel.qml, Repeater over _ai.planSummary) and the stage editor's completion-
// condition hint -- both wrapMode: Text.Wrap with no width cap, so their implicitWidth
// reported the full *unwrapped* line (289px and 217px respectively for this fixture's
// content) and a single-column ColumnLayout used that, uncapped, to override anchors.fill's
// ~132px available width, dragging every sibling row (including the field and the flat-plan
// editor's paramField at SwarmAIPanel.qml ~:379-392, the same pre-existing defect) out past
// the flickable's clip bounds with it. Also: Layout.minimumWidth, the property this test's
// own prior investigation assumed would fix it, measurably does nothing here; the property
// that actually caps a row's contribution to the shared column width is Layout.maximumWidth.
// The fix (SwarmAIPanel.qml) gives every wrapping QGCLabel and QGCTextField in the plan-
// confirm area (including the flat-plan editor, explicitly in scope per the task) a
// Layout.maximumWidth, and splits the one button row whose own combined width (~157px) was
// the next-largest offender into two rows so no control is truncated. A real
// QTest::mouseClick() (via clickButtonScrolled()) now lands on this button; the
// QEXPECT_FAIL that used to guard this known failure has been removed -- leaving it in
// place would make Qt report XPASS and fail the suite now that the click succeeds.
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

            QVERIFY(verifyEnabled(QStringLiteral("swarmStageActionApply"), true, QStringLiteral("stage apply button")));
            QVERIFY2(
                clickButtonScrolled(QStringLiteral("swarmStageActionApply"), QStringLiteral("swarmAIPanelFlickable")),
                "Could not click the stage apply button");

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
