import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

Rectangle {
    id: root

    property string title
    property bool expanded: true
    property bool fullScreen: false
    property bool floating: false
    property bool showSettingsButton: false
    property real defaultX: 0
    property real defaultY: 0
    property real defaultWidth: ScreenTools.defaultFontPixelWidth * 64
    property real maxHeight: 700
    property real contentHeight: 0
    property real _margin: ScreenTools.defaultFontPixelWidth / 2
    property var qgcPal: QGroundControl.globalPalette
    readonly property Item headerItem: headerRow
    readonly property real headerHeight: headerRow.implicitHeight + _margin * 2
    readonly property real expandedDockedHeight:
        Math.max(headerHeight, Math.min(contentHeight + headerHeight + _margin, maxHeight))
    readonly property real dockedHeight: expanded ? expandedDockedHeight : headerHeight
    readonly property real _viewportWidth: parent ? parent.width : defaultWidth
    readonly property real _viewportHeight: parent ? parent.height : maxHeight
    property rect _floatingGeometry: Qt.rect(0, 0, defaultWidth, maxHeight)
    property real _lastExpandedHeight: 0

    signal settingsRequested()
    signal raiseRequested()

    x: fullScreen ? 0 : Math.max(0, Math.min(floating ? _floatingGeometry.x : defaultX, _viewportWidth - width))
    y: fullScreen ? 0 : Math.max(0, Math.min(floating ? _floatingGeometry.y : defaultY, _viewportHeight - height))
    width: fullScreen ? _viewportWidth : Math.min(floating ? _floatingGeometry.width : defaultWidth, _viewportWidth)
    height: fullScreen ? _viewportHeight
                      : Math.min(expanded && floating ? _floatingGeometry.height : dockedHeight, _viewportHeight)
    radius: fullScreen ? 0 : _margin
    color: qgcPal.windowShade
    border.color: qgcPal.text
    border.width: 1
    opacity: fullScreen ? 1 : 0.95
    onHeightChanged: {
        if (expanded && !fullScreen && height > headerHeight) _lastExpandedHeight = height
    }

    function beginFloating() {
        if (!floating) {
            _floatingGeometry = Qt.rect(x, y, width,
                expanded ? height : Math.max(expandedDockedHeight, _lastExpandedHeight))
            floating = true
        }
        raiseRequested()
    }

    function movePanel(newX, newY) {
        _floatingGeometry = Qt.rect(Math.max(0, Math.min(newX, _viewportWidth - width)),
                                    Math.max(0, Math.min(newY, _viewportHeight - height)),
                                    _floatingGeometry.width, _floatingGeometry.height)
    }

    function resizePanel(newWidth, newHeight) {
        _floatingGeometry = Qt.rect(x, y,
            Math.min(_viewportWidth - x, Math.max(ScreenTools.defaultFontPixelWidth * 48, newWidth)),
            Math.min(_viewportHeight - y, Math.max(headerHeight + ScreenTools.defaultFontPixelHeight * 5, newHeight)))
    }

    function toggleFullScreen() {
        expanded = true
        fullScreen = !fullScreen
        raiseRequested()
    }

    function resetLayout() {
        fullScreen = false
        floating = false
        expanded = true
    }

    // Consume background input so moving/resizing a panel cannot operate the map underneath.
    MouseArea {
        anchors.fill: parent
        onWheel: (wheel) => { wheel.accepted = true }
    }

    TapHandler {
        onPressedChanged: if (pressed) root.raiseRequested()
    }

    RowLayout {
        id: headerRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: root._margin
        spacing: root._margin
        z: 1

        QGCLabel {
            text: root.title
            font.bold: true
            elide: Text.ElideRight
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.fillHeight: true
            verticalAlignment: Text.AlignVCenter

            MouseArea {
                id: moveHandle
                objectName: root.objectName + "DragHandle"
                anchors.fill: parent
                cursorShape: root.fullScreen ? Qt.ArrowCursor : Qt.SizeAllCursor
                preventStealing: true
                property point pressPosition
                property point panelPosition
                onPressed: (mouse) => {
                    pressPosition = mapToItem(root.parent, mouse.x, mouse.y)
                    panelPosition = Qt.point(root.x, root.y)
                    root.raiseRequested()
                }
                onPositionChanged: (mouse) => {
                    if (pressed && !root.fullScreen) {
                        const point = mapToItem(root.parent, mouse.x, mouse.y)
                        root.beginFloating()
                        root.movePanel(panelPosition.x + point.x - pressPosition.x,
                                       panelPosition.y + point.y - pressPosition.y)
                    }
                }
                onDoubleClicked: root.toggleFullScreen()
            }
        }
        QGCButton {
            text: qsTr("配置")
            visible: root.showSettingsButton
            onClicked: root.settingsRequested()
        }
        QGCButton {
            objectName: root.objectName + "Reset"
            text: qsTr("复位")
            ToolTip.text: qsTr("恢复面板默认位置和大小")
            ToolTip.visible: hovered
            onClicked: root.resetLayout()
        }
        QGCButton {
            objectName: root.objectName + "FullScreen"
            text: root.fullScreen ? qsTr("还原") : qsTr("全屏")
            onClicked: root.toggleFullScreen()
        }
        QGCButton {
            objectName: root.objectName + "Collapse"
            text: root.expanded ? qsTr("收起") : qsTr("展开")
            onClicked: {
                root.fullScreen = false
                root.expanded = !root.expanded
            }
        }
    }

    MouseArea {
        id: resizeHandle
        objectName: root.objectName + "ResizeHandle"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: ScreenTools.defaultFontPixelHeight * 1.5
        height: width
        visible: root.expanded && !root.fullScreen
        cursorShape: Qt.SizeFDiagCursor
        preventStealing: true
        z: 2
        property point pressPosition
        property size panelSize
        onPressed: (mouse) => {
            pressPosition = mapToItem(root.parent, mouse.x, mouse.y)
            panelSize = Qt.size(root.width, root.height)
            root.beginFloating()
        }
        onPositionChanged: (mouse) => {
            if (pressed) {
                const point = mapToItem(root.parent, mouse.x, mouse.y)
                root.resizePanel(panelSize.width + point.x - pressPosition.x,
                                 panelSize.height + point.y - pressPosition.y)
            }
        }
        QGCLabel {
            anchors.centerIn: parent
            text: "◢"
            color: root.qgcPal.text
        }
    }
}
