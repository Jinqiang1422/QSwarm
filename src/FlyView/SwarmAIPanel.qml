/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmAIPanel
 *
 * 大模型集群任务面板：
 *   - 接口配置（OpenAI 兼容端点：DeepSeek / Kimi / 通义千问 / OpenAI / Ollama 本地）
 *   - 自然语言指令输入与对话记录
 *   - AI 生成的行动计划预览与人工确认（人在回路）
 *
 * 集成：与 SwarmControlPanel 并列，在 FlyViewWidgetLayer.qml 中实例化。
 ****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.FlyView

Rectangle {
    id:     _root
    width:  ScreenTools.defaultFontPixelWidth * 24
    height: expanded ? Math.min(mainColumn.implicitHeight + _margin * 2, maxHeight) : headerRow.implicitHeight + _margin * 2
    radius: ScreenTools.defaultFontPixelWidth / 2
    color:  qgcPal.windowShade
    border.color: qgcPal.text
    border.width: 1
    opacity: 0.95

    property bool expanded:   true
    property real maxHeight:  700
    property real _margin:    ScreenTools.defaultFontPixelWidth / 2
    property var  qgcPal:     QGroundControl.globalPalette
    property var  _swarm:     QGroundControl.swarmManager
    property var  _ai:        QGroundControl.swarmManager.aiAgent
    property var  _task:      QGroundControl.swarmManager.taskController
    property var  _lib:       QGroundControl.swarmManager.taskLibrary

    // ---------------------------------------------------------------- 标题栏
    RowLayout {
        id:             headerRow
        anchors.left:   parent.left
        anchors.right:  parent.right
        anchors.top:    parent.top
        anchors.margins: _margin
        spacing:        _margin

        QGCLabel {
            text:               qsTr("AI 集群指挥") + (_ai.busy ? qsTr("（思考中…）") : "")
            font.bold:          true
            Layout.fillWidth:   true
        }
        QGCButton {
            text:       qsTr("配置")
            onClicked:  configColumn.visible = !configColumn.visible
        }
        QGCButton {
            text:       _root.expanded ? qsTr("收起") : qsTr("展开")
            onClicked:  _root.expanded = !_root.expanded
        }
    }

    // ---------------------------------------------------------------- 主内容
    QGCFlickable {
        anchors.left:           parent.left
        anchors.right:          parent.right
        anchors.top:            headerRow.bottom
        anchors.bottom:         parent.bottom
        anchors.margins:        _margin
        contentWidth:           mainColumn.width
        contentHeight:          mainColumn.height
        visible:                _root.expanded
        clip:                   true

        Column {
            id:     mainColumn
            width:  _root.width - _margin * 2
            spacing: _margin

            // ==================== 接口配置（默认折叠） ====================
            Column {
                id:             configColumn
                visible:        false
                spacing:        _margin
                width:          parent.width

                QGCLabel { text: qsTr("◆ 大模型接口（OpenAI 兼容）"); font.bold: true }
                QGCLabel { text: qsTr("接口地址") }
                QGCTextField {
                    width: parent.width
                    text: _ai.apiBaseUrl
                    placeholderText: "https://api.deepseek.com/v1"
                    onEditingFinished: _ai.apiBaseUrl = text
                }
                QGCLabel { text: qsTr("API Key") }
                QGCTextField {
                    width: parent.width
                    text: _ai.apiKey
                    echoMode: TextInput.Password
                    onEditingFinished: _ai.apiKey = text
                }
                QGCLabel { text: qsTr("模型") }
                QGCTextField {
                    width: parent.width
                    text: _ai.modelName
                    placeholderText: "deepseek-chat"
                    onEditingFinished: _ai.modelName = text
                }
                RowLayout {
                    width:      parent.width
                    spacing: _margin
                    QGCButton { text: qsTr("测试连接"); onClicked: _ai.testConnection() }
                    QGCCheckBox {
                        text:    qsTr("危险动作需人工确认")
                        checked: _ai.requireConfirm
                        onClicked: _ai.requireConfirm = checked
                    }
                }
                QGCCheckBox {
                    text:    qsTr("工具调用（Function Calling，端点不支持时关闭）")
                    checked: _ai.useFunctionCalling
                    onClicked: _ai.useFunctionCalling = checked
                }

                // ---------- 自主等级（WP1.3） ----------
                QGCLabel { text: qsTr("◆ 自主等级"); font.bold: true }
                RowLayout {
                    width:      parent.width
                    spacing: _margin
                    QGCComboBox {
                        model:          [qsTr("L1 仅建议"), qsTr("L2 监督执行"), qsTr("L3 限定域自主")]
                        currentIndex:   _swarm.autonomyLevel - 1
                        onActivated:    _swarm.autonomyLevel = currentIndex + 1
                    }
                    QGCLabel {
                        Layout.fillWidth:   true
                        text:               _swarm.autonomyLevelDescription()
                        wrapMode:           Text.WordWrap
                        font.pointSize:     ScreenTools.smallFontPointSize
                    }
                }
                Flow {
                    width:              parent.width
                    spacing:            _margin
                    visible:            _swarm.autonomyLevel === 3
                    Repeater {
                        model: ["arm", "takeoff", "land", "rtl", "start_mission", "disarm", "start_formation"]
                        delegate: QGCCheckBox {
                            required property string modelData
                            text:    modelData
                            checked: _swarm.l3AllowedTypes.indexOf(modelData) >= 0
                            onClicked: {
                                var types = _swarm.l3AllowedTypes.slice()
                                var idx = types.indexOf(modelData)
                                if (checked && idx < 0) { types.push(modelData) }
                                if (!checked && idx >= 0) { types.splice(idx, 1) }
                                _swarm.l3AllowedTypes = types
                            }
                        }
                    }
                    QGCLabel {
                        width:              parent.width
                        text:               qsTr("L3 白名单：勾选的危险动作免确认自动执行（紧急上锁永不自动）")
                        wrapMode:           Text.WordWrap
                        font.pointSize:     ScreenTools.smallFontPointSize
                    }
                }

                // ---------- 审计（WP1.1） ----------
                RowLayout {
                    width:      parent.width
                    spacing: _margin
                    QGCLabel { text: qsTr("◆ 审计追溯"); font.bold: true }
                    QGCButton {
                        text:       qsTr("导出当前会话")
                        onClicked:  _swarm.exportAudit()
                    }
                }
                QGCLabel {
                    width:              parent.width
                    text:               qsTr("会话 %1｜已记录 %2 条｜%3")
                                            .arg(_swarm.auditSessionId())
                                            .arg(_swarm.auditEventCount())
                                            .arg(_swarm.auditDir)
                    wrapMode:           Text.WrapAnywhere
                    font.pointSize:     ScreenTools.smallFontPointSize
                }
                QGCLabel {
                    width:              parent.width
                    text:               qsTr("上次响应 %1s｜围栏：%2")
                                            .arg(_ai.responseTimeSec.toFixed(1))
                                            .arg(_swarm.fenceSummary())
                    wrapMode:           Text.WrapAnywhere
                    font.pointSize:     ScreenTools.smallFontPointSize
                }

                // ---- P3 感知链路 / 态势 / 周期摘要 ----
                Rectangle {
                    width:              parent.width
                    height:             p3Column.height + _margin * 2
                    color:              "#1a2e3a"
                    radius:             4
                    border.color:       _swarm.perception.serviceOnline ? "#2ecc71" : "#546e7a"
                    border.width:       1

                    ColumnLayout {
                        id:                 p3Column
                        anchors.left:       parent.left
                        anchors.right:      parent.right
                        anchors.top:        parent.top
                        anchors.margins:    _margin
                        spacing:            _margin / 2

                        RowLayout {
                            spacing: _margin
                            QGCLabel {
                                text:  _swarm.perception.serviceOnline
                                       ? qsTr("● 感知链路在线｜心跳 %1Hz｜目标 %2 个｜线路时延 %3ms")
                                            .arg(_swarm.perception.heartbeatHz.toFixed(1))
                                            .arg(_swarm.perception.lastTrackCount)
                                            .arg(_swarm.perception.wireLatencyMs.toFixed(0))
                                       : qsTr("○ 感知链路待机（端口 %1）").arg(_swarm.perception.listenPort)
                                color: _swarm.perception.serviceOnline ? "#2ecc71" : "#90A4AE"
                                font.pointSize: ScreenTools.smallFontPointSize
                            }
                            QGCButton {
                                text:       _swarm.perception.listening ? qsTr("停止") : qsTr("启动")
                                onClicked:  _swarm.perception.listening ? _swarm.stopPerception()
                                                                        : _swarm.startPerception()
                            }
                        }
                        QGCLabel {
                            Layout.fillWidth:   true
                            text:               qsTr("态势：%1").arg(_swarm.situationOneLiner)
                            wrapMode:           Text.Wrap
                            color:              "#4fc3f7"
                            font.pointSize:     ScreenTools.smallFontPointSize
                        }
                        RowLayout {
                            spacing: _margin
                            QGCCheckBox {
                                id:      sitSummaryToggle
                                text:    qsTr("周期态势摘要")
                                checked: _ai.situationSummaryEnabled
                                onClicked: _ai.situationSummaryEnabled = checked
                            }
                            QGCLabel { text: qsTr("间隔(s)"); font.pointSize: ScreenTools.smallFontPointSize }
                            QGCTextField {
                                Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 6
                                text:                   _ai.situationIntervalSec.toString()
                                onEditingFinished:      _ai.situationIntervalSec = parseInt(text) || 60
                            }
                        }
                    }
                }

                // ---- P3 告警区（事件检测引擎） ----
                ColumnLayout {
                    width:              parent.width
                    visible:            _swarm.alerts.length > 0
                    spacing:            _margin / 2

                    QGCLabel { text: qsTr("◆ 告警（%1）").arg(_swarm.alerts.length); font.bold: true }
                    Repeater {
                        model: Math.min(_swarm.alerts.length, 5)
                        delegate: QGCLabel {
                            Layout.fillWidth:   true
                            text:               _swarm.alerts[index].text
                            wrapMode:           Text.Wrap
                            color:              _swarm.alerts[index].level === "critical" ? "#e53935"
                                              : _swarm.alerts[index].level === "warning"  ? "#fb8c00"
                                                                                          : "#4fc3f7"
                            font.pointSize:     ScreenTools.smallFontPointSize
                        }
                    }
                }
            }

            // ==================== 对话记录 ====================
            QGCLabel { text: qsTr("◆ 对话"); font.bold: true }

            Rectangle {
                width:                  parent.width
                height:                 ScreenTools.defaultFontPixelHeight * 12
                color:                  qgcPal.window
                radius:                 4

                ListView {
                    id:           chatView
                    anchors.fill: parent
                    anchors.margins: _margin / 2
                    clip:         true
                    spacing:      _margin / 2
                    model:        _ai.chatLog
                    onCountChanged: positionViewAtEnd()

                    delegate: QGCLabel {
                        width:          chatView.width
                        text:           modelData
                        wrapMode:       Text.Wrap
                        color:          modelData.indexOf("[你]") === 0 ? "#4fc3f7"
                                      : modelData.indexOf("[AI]") === 0 ? qgcPal.text
                                      : "#f0c040"
                    }
                }
            }

            // ==================== 指令输入 ====================
            RowLayout {
                width:   parent.width
                spacing: _margin
                QGCTextField {
                    id:                 inputField
                    Layout.fillWidth:   true
                    placeholderText:    qsTr("输入集群任务，如：全体起飞到30米后楔形编队")
                    enabled:            !_ai.busy
                    onAccepted:         sendButton.clicked()
                }
                QGCButton {
                    id:         sendButton
                    text:       qsTr("发送")
                    enabled:    !_ai.busy && inputField.text.trim() !== ""
                    onClicked: {
                        _ai.sendText(inputField.text,
                                     QGroundControl.flightMapPosition.latitude,
                                     QGroundControl.flightMapPosition.longitude)
                        inputField.text = ""
                    }
                }
                QGCButton {
                    text:       qsTr("新会话")
                    onClicked:  _ai.clearHistory()
                }
            }

            // ==================== 计划确认区 ====================
            Rectangle {
                visible:                _ai.planPending
                width:                  parent.width
                implicitHeight:         planColumn.implicitHeight + _margin * 2
                color:                  _ai.planHasDangerous ? "#4d2c2c" : qgcPal.window
                border.color:           _ai.planHasDangerous ? "#e74c3c" : qgcPal.buttonHighlight
                border.width:           1
                radius:                 4

                ColumnLayout {
                    id:             planColumn
                    anchors.fill:   parent
                    anchors.margins: _margin
                    spacing:        _margin / 2

                    QGCLabel {
                        text:       _ai.planHasDangerous ? qsTr("⚠ AI 行动计划（含危险动作，请确认）")
                                                         : qsTr("AI 行动计划")
                        font.bold:  true
                        color:      _ai.planHasDangerous ? "#ff8080" : qgcPal.text
                    }
                    Repeater {
                        model: _ai.planSummary
                        delegate: ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            QGCLabel {
                                text:       "  %1. %2".arg(index + 1).arg(modelData)
                                wrapMode:   Text.Wrap
                                Layout.fillWidth: true
                            }
                            RowLayout {
                                visible:    editParamsToggle.checked
                                spacing:    _margin / 2
                                QGCTextField {
                                    id:                 paramField
                                    Layout.fillWidth:   true
                                    text:               (_ai.pendingPlanParamsJson[index] !== undefined) ? _ai.pendingPlanParamsJson[index] : ""
                                    placeholderText:    qsTr("参数 JSON，如 {\"altitude\": 40}")
                                }
                                QGCButton {
                                    text:       qsTr("更新")
                                    onClicked:  _ai.updatePendingAction(index, paramField.text)
                                }
                            }
                        }
                    }
                    RowLayout {
                        spacing: _margin
                        QGCButton { text: qsTr("确认执行"); onClicked: _ai.confirmPlan() }
                        QGCButton { text: qsTr("放弃");     onClicked: _ai.rejectPlan() }
                        QGCCheckBox {
                            id:      editParamsToggle
                            text:    qsTr("编辑参数")
                            checked: false
                        }
                    }
                }
            }

            // ==================== 候选方案区（WP2.2 多候选） ====================
            Rectangle {
                visible:                _ai.candidates.length > 0
                width:                  parent.width
                implicitHeight:         candColumn.implicitHeight + _margin * 2
                color:                  qgcPal.window
                border.color:           "#4fc3f7"
                border.width:           1
                radius:                 4

                ColumnLayout {
                    id:             candColumn
                    anchors.fill:   parent
                    anchors.margins: _margin
                    spacing:        _margin / 2

                    QGCLabel { text: qsTr("◆ 候选方案对比（选择其一执行）"); font.bold: true }
                    Repeater {
                        model: _ai.candidates
                        delegate: Rectangle {
                            Layout.fillWidth: true
                            implicitHeight:   candItemCol.implicitHeight + _margin
                            color:            "#2c3e50"
                            radius:           4
                            ColumnLayout {
                                id:                 candItemCol
                                anchors.left:       parent.left
                                anchors.right:      parent.right
                                anchors.top:        parent.top
                                anchors.margins:    _margin / 2
                                spacing:            2
                                QGCLabel {
                                    text:       qsTr("%1. %2（%3）%4")
                                                    .arg(modelData.index + 1)
                                                    .arg(modelData.title)
                                                    .arg(modelData.kind)
                                                    .arg(modelData.error_count > 0
                                                         ? qsTr(" ⚠%1项违例").arg(modelData.error_count) : "")
                                    font.bold:  true
                                    wrapMode:   Text.Wrap
                                    Layout.fillWidth: true
                                }
                                QGCLabel {
                                    text:       modelData.summary
                                    wrapMode:   Text.Wrap
                                    Layout.fillWidth: true
                                    font.pointSize: ScreenTools.smallFontPointSize
                                }
                                QGCLabel {
                                    text:       qsTr("优：%1").arg(modelData.pros)
                                    wrapMode:   Text.Wrap
                                    Layout.fillWidth: true
                                    font.pointSize: ScreenTools.smallFontPointSize
                                    color:      "#2ecc71"
                                }
                                QGCLabel {
                                    text:       qsTr("劣：%1").arg(modelData.cons)
                                    wrapMode:   Text.Wrap
                                    Layout.fillWidth: true
                                    font.pointSize: ScreenTools.smallFontPointSize
                                    color:      "#e67e22"
                                }
                                QGCLabel {
                                    text:       qsTr("预演：%1").arg(modelData.estimate)
                                    wrapMode:   Text.Wrap
                                    Layout.fillWidth: true
                                    font.pointSize: ScreenTools.smallFontPointSize
                                    color:      "#4fc3f7"
                                }
                                QGCButton {
                                    text:       qsTr("选择此方案")
                                    onClicked:  _ai.selectCandidate(modelData.index)
                                }
                            }
                        }
                    }
                    QGCButton { text: qsTr("全部放弃"); onClicked: _ai.rejectCandidates() }
                }
            }

            // ==================== 多阶段任务状态区 ====================
            Rectangle {
                visible:                _task.running || _task.stateText !== qsTr("空闲")
                width:                  parent.width
                implicitHeight:         taskColumn.implicitHeight + _margin * 2
                color:                  qgcPal.window
                border.color:           _task.running ? "#4fc3f7" : qgcPal.buttonHighlight
                border.width:           1
                radius:                 4

                ColumnLayout {
                    id:             taskColumn
                    anchors.fill:   parent
                    anchors.margins: _margin
                    spacing:        _margin / 2

                    QGCLabel {
                        text:       qsTr("◆ 任务状态机：%1").arg(_task.taskName)
                        font.bold:  true
                    }
                    QGCLabel {
                        text:           _task.stateText
                        wrapMode:       Text.Wrap
                        Layout.fillWidth: true
                        color:          _task.running ? "#4fc3f7" : qgcPal.text
                    }
                    Repeater {
                        model: _task.stageNames
                        delegate: QGCLabel {
                            text: "%1 %2".arg(
                                      index < _task.currentStageIndex  ? "✓" :
                                      index === _task.currentStageIndex ? "▶" : "○")
                                  .arg(modelData)
                            color: index < _task.currentStageIndex   ? "#7ec87e" :
                                   index === _task.currentStageIndex ? "#4fc3f7" : qgcPal.text
                        }
                    }
                    RowLayout {
                        spacing: _margin
                        QGCButton {
                            text:       qsTr("继续（人工确认）")
                            visible:    _task.waitingForProceed
                            onClicked:  _task.proceed()
                        }
                        QGCButton {
                            text:       qsTr("中止任务")
                            enabled:    _task.running
                            onClicked:  _task.abort(qsTr("操作员手动中止"))
                        }
                    }
                }
            }

            // ==================== 任务模板库 ====================
            RowLayout {
                width:   parent.width
                spacing: _margin
                QGCLabel { text: qsTr("◆ 任务模板库（%1 套）").arg(_lib.templates.length); font.bold: true; Layout.fillWidth: true }
                QGCButton { text: qsTr("刷新"); onClicked: _lib.reload() }
                QGCButton { text: qsTr("打开目录"); onClicked: _lib.openTemplateDir() }
            }

            Repeater {
                model: _lib.templates

                delegate: Rectangle {
                    id:                 tplDelegate
                    width:              mainColumn.width
                    implicitHeight:     tplColumn.implicitHeight + _margin * 2
                    color:              qgcPal.window
                    border.color:       qgcPal.buttonHighlight
                    border.width:       1
                    radius:             4

                    property var _tpl:         modelData
                    property var _paramValues: ({})

                    ColumnLayout {
                        id:             tplColumn
                        anchors.fill:   parent
                        anchors.margins: _margin
                        spacing:        _margin / 2

                        RowLayout {
                            spacing: _margin
                            QGCLabel {
                                text: "%1（%2 阶段）%3".arg(tplDelegate._tpl.name)
                                      .arg(tplDelegate._tpl.stageCount)
                                      .arg(tplDelegate._tpl.builtIn ? " [内置]" : "")
                                font.bold: true
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }
                            QGCButton {
                                text: qsTr("执行")
                                onClicked: _lib.runTemplate(tplDelegate._tpl.name,
                                                            tplDelegate._paramValues,
                                                            QGroundControl.flightMapPosition.latitude,
                                                            QGroundControl.flightMapPosition.longitude)
                            }
                            QGCButton {
                                text:    qsTr("删除")
                                visible: !tplDelegate._tpl.builtIn
                                onClicked: _lib.deleteTemplate(tplDelegate._tpl.name)
                            }
                        }
                        QGCLabel {
                            text:           tplDelegate._tpl.description
                            wrapMode:       Text.Wrap
                            Layout.fillWidth: true
                            color:          qgcPal.text
                            opacity:        0.8
                        }

                        // 参数表单（按模板 parameters 动态生成）
                        Repeater {
                            model: tplDelegate._tpl.parameters
                            delegate: RowLayout {
                                spacing: _margin
                                QGCLabel {
                                    text: modelData.label
                                    Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 9
                                    elide: Text.ElideRight
                                }
                                QGCTextField {
                                    Layout.fillWidth: true
                                    text: modelData.default
                                    onEditingFinished: {
                                        var t = modelData.type
                                        tplDelegate._paramValues[modelData.key] =
                                            (t === "number" || t === "integer") ? parseFloat(text) : text
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
