#pragma once

#include "QmlUITestBase.h"

/// UI smoke test for the SwarmAIPanel stage editor (Task 7). Confirms:
///   - the per-action edit field is actually visible and usably wide once
///     "编辑参数" is enabled (the geometry bug the Task 7 review caught);
///   - an edit made through the field/apply button round-trips into
///     SwarmAIAgent::pendingTaskModel();
///   - a set_selection action's field stays disabled (changing the target
///     machine mid-task is rejected by design, see SwarmAIAgent::
///     updatePendingStageAction()).
class SwarmTaskEditorUITest : public QmlUITestBase
{
    Q_OBJECT

private slots:
    void init() override;
    void cleanup() override;

    void _testStageActionFieldIsVisibleAndWide();
    void _testEditRoundTripsThroughUI();
    void _testSetSelectionActionFieldDisabled();

private:
    /// Grows the offscreen test window so SwarmAIPanel's QGCFlickable gets a positive
    /// height once SwarmControlPanel is also visible above it. See the definition for why.
    void _growWindowForPanel();

    /// Waits for the named QGCFlickable's contentHeight to stop changing across
    /// consecutive polls. See the definition for why this is needed.
    bool _waitForContentHeightSettled(const QString& flickableObjectName);
};
