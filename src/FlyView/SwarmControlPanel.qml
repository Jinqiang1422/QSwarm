/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmControlPanel
 *
 * Fly View 集群控制面板：
 *   1. 多机实时监控（选中/解锁状态/飞行模式/高度/地速/电量）
 *   2. 集群一键指令（解锁、起飞、降落、返航、暂停、开始任务、紧急上锁）
 *   3. 编队飞行控制（队形、间距、高度分层、长机选择、启停）
 *   4. 集群任务规划（矩形区域覆盖：长宽/角度/高度/行距 → 生成并上传）
 *
 * 集成：在 FlyViewWidgetLayer.qml 中实例化（见《集成补丁说明.md》）。
 ****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import QGroundControl
import QGroundControl.Controls
import QGroundControl.FlyView

SwarmPanel {
    id: _root
    contentHeight: mainColumn.implicitHeight
    property var  _swarm:     QGroundControl.swarmManager
    property var  _mvm:       QGroundControl.multiVehicleManager

    title: qsTr("集群指挥控制 (%1 架)").arg(_mvm.vehicles.count)

    // ---------------------------------------------------------------- 主内容
    QGCFlickable {
        anchors.left:           parent.left
        anchors.right:          parent.right
        anchors.top:            _root.headerItem.bottom
        anchors.bottom:         parent.bottom
        anchors.margins:        _margin
        anchors.bottomMargin:   _margin + ScreenTools.defaultFontPixelHeight
        contentWidth:           mainColumn.width
        contentHeight:          mainColumn.height
        visible:                _root.expanded
        clip:                   true

        ColumnLayout {
            id:     mainColumn
            width:  _root.width - _margin * 2
            spacing: _margin

            // ==================== 1. 多机实时监控 ====================
            QGCLabel { text: qsTr("◆ 多机实时监控（点击行选中/取消）"); font.bold: true }

            Repeater {
                model: _mvm.vehicles

                delegate: Rectangle {
                    Layout.fillWidth:   true
                    implicitHeight:     vehicleRow.implicitHeight + _margin
                    radius:             4
                    color:              _isSelected ? qgcPal.buttonHighlight : qgcPal.button
                    border.color:       (_mvm.activeVehicle === _vehicle) ? "green" : "transparent"
                    border.width:       2

                    property var  _vehicle:    object
                    property bool _isSelected: {
                        for (var i = 0; i < _mvm.selectedVehicles.count; i++) {
                            if (_mvm.selectedVehicles.get(i).id === _vehicle.id) return true
                        }
                        return false
                    }

                    QGCMouseArea {
                        anchors.fill: parent
                        onClicked: {
                            if (parent._isSelected) _mvm.deselectVehicle(parent._vehicle.id)
                            else                    _mvm.selectVehicle(parent._vehicle.id)
                        }
                    }

                    RowLayout {
                        id:             vehicleRow
                        anchors.fill:   parent
                        anchors.margins: _margin / 2
                        spacing:        _margin

                        QGCLabel { text: _vehicle.id; font.bold: true }
                        QGCLabel { text: _vehicle.armed ? qsTr("已解锁") : qsTr("已上锁"); color: _vehicle.armed ? "#e74c3c" : qgcPal.text }
                        QGCLabel { text: _vehicle.flightMode; Layout.fillWidth: true; elide: Text.ElideRight }
                        QGCLabel { text: qsTr("高 %1").arg(_vehicle.vehicle.altitudeRelative.valueString) }
                        QGCLabel { text: qsTr("速 %1").arg(_vehicle.vehicle.groundSpeed.valueString) }
                        QGCLabel {
                            text: _vehicle.batteries.count > 0
                                  ? qsTr("电 %1%").arg(_vehicle.batteries.get(0).percentRemaining.valueString)
                                  : qsTr("电 --")
                        }
                    }
                }
            }

            RowLayout {
                spacing: _margin
                QGCButton {
                    text: qsTr("全选")
                    onClicked: {
                        for (var i = 0; i < _mvm.vehicles.count; i++)
                            _mvm.selectVehicle(_mvm.vehicles.get(i).id)
                    }
                }
                QGCButton { text: qsTr("全不选"); onClicked: _mvm.deselectAllVehicles() }
                QGCLabel  { text: qsTr("已选 %1 架").arg(_mvm.selectedVehicles.count); Layout.fillWidth: true }
            }

            // ==================== 2. 集群一键指令 ====================
            QGCLabel { text: qsTr("◆ 集群一键指令"); font.bold: true }

            RowLayout {
                spacing: _margin
                QGCLabel { text: qsTr("起飞高度(米)") }
                QGCTextField {
                    id: takeoffAltField
                    Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 6
                    text: "10"
                    validator: DoubleValidator { bottom: 2; top: 120 }
                }
            }

            GridLayout {
                columns: 4
                rowSpacing: _margin
                columnSpacing: _margin

                QGCButton { text: qsTr("解锁");   onClicked: _swarm.armAll(true) }
                QGCButton { text: qsTr("上锁");   onClicked: _swarm.armAll(false) }
                QGCButton { text: qsTr("起飞");   onClicked: takeoffConfirm.open() }
                QGCButton { text: qsTr("降落");   onClicked: landConfirm.open() }
                QGCButton { text: qsTr("返航");   onClicked: rtlConfirm.open() }
                QGCButton { text: qsTr("暂停");   onClicked: _swarm.pauseAll() }
                QGCButton { text: qsTr("开始任务"); onClicked: _swarm.startMissionAll() }
                QGCButton {
                    text: qsTr("紧急上锁")
                    onClicked: emergencyConfirm.open()
                }
            }

            // ==================== 3. 编队飞行 ====================
            QGCLabel { text: qsTr("◆ 编队飞行"); font.bold: true }

            GridLayout {
                columns: 2
                rowSpacing: _margin
                columnSpacing: _margin

                QGCLabel { text: qsTr("队形") }
                QGCComboBox {
                    id: formationTypeCombo
                    Layout.fillWidth: true
                    model: [ qsTr("楔形"), qsTr("纵队"), qsTr("横队"), qsTr("环形") ]
                    onActivated: _swarm.formation.formationType = currentIndex
                }

                QGCLabel { text: qsTr("长机") }
                QGCComboBox {
                    id: leaderCombo
                    Layout.fillWidth: true
                    model: {
                        var ids = []
                        for (var i = 0; i < _mvm.vehicles.count; i++)
                            ids.push(qsTr("载具 %1").arg(_mvm.vehicles.get(i).id))
                        return ids
                    }
                }

                QGCLabel { text: qsTr("间距(米)") }
                QGCTextField {
                    id: spacingField
                    Layout.fillWidth: true
                    text: "10"
                    validator: DoubleValidator { bottom: 2; top: 500 }
                    onEditingFinished: _swarm.formation.spacingMeters = parseFloat(text)
                }

                QGCLabel { text: qsTr("高度分层(米)") }
                QGCTextField {
                    id: vstepField
                    Layout.fillWidth: true
                    text: "3"
                    validator: DoubleValidator { bottom: 0; top: 50 }
                    onEditingFinished: _swarm.formation.verticalStepMeters = parseFloat(text)
                }
            }

            RowLayout {
                spacing: _margin
                QGCButton {
                    text:    qsTr("开始编队")
                    enabled: !_swarm.formation.active && _mvm.vehicles.count > 1
                    onClicked: {
                        if (_mvm.vehicles.count > 0) {
                            var leaderId = _mvm.vehicles.get(Math.max(0, leaderCombo.currentIndex)).id
                            _swarm.formation.formationType = formationTypeCombo.currentIndex
                            _swarm.startFormation(leaderId)
                        }
                    }
                }
                QGCButton {
                    text:    qsTr("停止编队")
                    enabled: _swarm.formation.active
                    onClicked: _swarm.stopFormation()
                }
                QGCLabel {
                    text: _swarm.formation.statusText
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
            }

            // ==================== 4. 集群任务规划 ====================
            QGCLabel { text: qsTr("◆ 区域覆盖任务（中心取当前地图中心）"); font.bold: true }

            GridLayout {
                columns: 4
                rowSpacing: _margin
                columnSpacing: _margin

                QGCLabel { text: qsTr("长(米)") }
                QGCTextField { id: areaLengthField; text: "200"; Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 6; validator: DoubleValidator { bottom: 10; top: 100000 } }
                QGCLabel { text: qsTr("宽(米)") }
                QGCTextField { id: areaWidthField;  text: "120"; Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 6; validator: DoubleValidator { bottom: 10; top: 100000 } }

                QGCLabel { text: qsTr("角度(度)") }
                QGCTextField { id: areaAngleField;  text: "0";  Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 6; validator: DoubleValidator { bottom: 0; top: 359 } }
                QGCLabel { text: qsTr("行距(米)") }
                QGCTextField { id: spacingPlanField; text: "20"; Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 6; validator: DoubleValidator { bottom: 2; top: 1000 } }
            }

            RowLayout {
                spacing: _margin
                QGCButton {
                    text: qsTr("生成并上传")
                    onClicked: {
                        _swarm.missionPlanner.surveySpacing = parseFloat(spacingPlanField.text)
                        _swarm.planAndUploadCoverage(
                            QGroundControl.flightMapPosition,
                            parseFloat(areaLengthField.text),
                            parseFloat(areaWidthField.text),
                            parseFloat(areaAngleField.text))
                    }
                }
                QGCButton { text: qsTr("清空任务"); onClicked: _swarm.clearAllMissions() }
                QGCButton { text: qsTr("开始任务"); onClicked: _swarm.startMissionAll() }
            }
        }
    }

    // ---------------------------------------------------------------- 确认对话框
    MessageDialog {
        id: takeoffConfirm
        title: qsTr("集群起飞")
        text: qsTr("确认全体（或选中）无人机起飞至 %1 米？").arg(takeoffAltField.text)
        buttons: MessageDialog.Yes | MessageDialog.No
        onAccepted: _swarm.takeoffAll(parseFloat(takeoffAltField.text))
    }
    MessageDialog {
        id: landConfirm
        title: qsTr("集群降落")
        text: qsTr("确认全体（或选中）无人机原地降落？")
        buttons: MessageDialog.Yes | MessageDialog.No
        onAccepted: _swarm.landAll()
    }
    MessageDialog {
        id: rtlConfirm
        title: qsTr("集群返航")
        text: qsTr("确认全体（或选中）无人机返航？")
        buttons: MessageDialog.Yes | MessageDialog.No
        onAccepted: _swarm.rtlAll()
    }
    MessageDialog {
        id: emergencyConfirm
        title: qsTr("紧急上锁")
        text: qsTr("警告：飞行中强制上锁将导致坠机！仅在地面紧急情况下使用。确认继续？")
        buttons: MessageDialog.Yes | MessageDialog.No
        onAccepted: _swarm.emergencyDisarmAll()
    }
}
