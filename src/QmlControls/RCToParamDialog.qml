import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Dialogs

import QGroundControl
import QGroundControl.Controls
import QGroundControl.FactControls

QGCPopupDialog {
    id: root
    title:      qsTr("RC To Param")
    buttons:    Dialog.Cancel | Dialog.Ok

    property alias tuningFact: controller.tuningFact
    property string validationError: ""

    acceptButtonEnabled: controller.ready
    onAccepted: {
        preventClose = !controller.apply(scale.text, centerValue.text, tuningID.currentIndex, minValue.text, maxValue.text)
        validationError = preventClose ? qsTr("Check the vehicle connection and numeric limits. Min ≤ Center ≤ Max is required.") : ""
    }

    RCToParamDialogController {
        id: controller
    }

    ColumnLayout {
        spacing: ScreenTools.defaultDialogControlSpacing

        QGCLabel {
            Layout.preferredWidth:  mainGrid.width
            Layout.fillWidth:       true
            wrapMode:               Text.WordWrap
            text:                   qsTr("Bind an RC Channel to a parameter value. Tuning IDs can be mapped to an RC Channel from Radio Setup page.")
        }

        QGCLabel {
            Layout.preferredWidth:  mainGrid.width
            Layout.fillWidth:       true
            text:                   qsTr("Waiting on parameter update from Vehicle.")
            visible:                !controller.ready
        }

        GridLayout {
            id:             mainGrid
            columns:        2
            rowSpacing:     ScreenTools.defaultDialogControlSpacing
            columnSpacing:  ScreenTools.defaultDialogControlSpacing
            enabled:        controller.ready

            QGCLabel { text: qsTr("Parameter") }
            QGCLabel { text: root.tuningFact ? root.tuningFact.name : qsTr("Vehicle or parameter unavailable") }

            QGCLabel { text: qsTr("Tuning ID") }
            QGCComboBox {
                id:                 tuningID
                Layout.fillWidth:   true
                currentIndex:       0
                model:              [ 1, 2, 3 ]
            }

            QGCLabel { text: qsTr("Scale") }
            QGCTextField {
                id:     scale
                text:   controller.scale.valueString
            }

            QGCLabel { text: qsTr("Center Value") }
            QGCTextField {
                id:     centerValue
                text:   controller.center.valueString
            }

            QGCLabel { text: qsTr("Min Value") }
            QGCTextField {
                id:     minValue
                text:   controller.min.valueString
            }

            QGCLabel { text: qsTr("Max Value") }
            QGCTextField {
                id:     maxValue
                text:   controller.max.valueString
            }
        }

        QGCLabel {
            Layout.fillWidth: true
            Layout.preferredWidth: mainGrid.width
            wrapMode: Text.WordWrap
            visible: root.validationError.length > 0
            text: root.validationError
        }

        QGCLabel {
            Layout.preferredWidth:  mainGrid.width
            Layout.fillWidth:       true
            wrapMode:               Text.WordWrap
            text:                   qsTr("Double check that all values are correct prior to confirming dialog.")
        }
    }
}
