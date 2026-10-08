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
import QtQuick.Dialogs

import QGroundControl
import QGroundControl.Controls
import QGroundControl.FlyView

SwarmPanel {
    id: _root
    contentHeight: mainColumn.implicitHeight
    property var  _swarm:     QGroundControl.swarmManager
    property var  _ai:        QGroundControl.swarmManager.aiAgent
    property var  _task:      QGroundControl.swarmManager.taskController
    property var  _lib:       QGroundControl.swarmManager.taskLibrary

    title: qsTr("AI 集群指挥") + (_ai.busy ? qsTr("（思考中…）") : "")
    showSettingsButton: true
    onSettingsRequested: configColumn.visible = !configColumn.visible

    // ---------------------------------------------------------------- 主内容
    QGCFlickable {
        objectName:             "swarmAIPanelFlickable"
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

                // 交付锁定提示：仅在 QGC_SWARM_AI_AUTODISPATCH 关闭的构建中非空（见 SwarmManager::
                // deliveryNotice）。放在配置区顶部、常驻显示，不随下方自主等级等控件的隐藏而消失。
                // 宽度处理：configColumn 是普通 Column（非 ColumnLayout），沿用本区块内其余换行
                // QGCLabel 的既有写法——显式 width: parent.width，而非 Layout.fillWidth /
                // Layout.maximumWidth（那两者只在 RowLayout/ColumnLayout 内生效，Column 会忽略
                // Layout 附加属性）。Column 按子项的显式 width 分配空间、不会像单列 ColumnLayout
                // 那样用子项未换行的 implicitWidth 覆盖父级宽度，因此这里不会重演 Task 7c 撑宽面板
                // 导致按钮点不到的问题。
                QGCLabel {
                    text:       _swarm.deliveryNotice
                    visible:    _swarm.deliveryNotice !== ""
                    wrapMode:   Text.WordWrap
                    color:      qgcPal.warningText
                    width:      parent.width
                }

                QGCLabel { text: qsTr("◆ 大模型接口（OpenAI 兼容）"); font.bold: true }
                QGCLabel { text: qsTr("接口地址") }
                QGCTextField {
                    width: parent.width
                    text: _ai.apiBaseUrl
                    placeholderText: "https://api.deepseek.com/v1"
                    onEditingFinished: _ai.apiBaseUrl = text
                }
                QGCLabel { text: qsTr("API Key（仅本次会话）") }
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
                        // 交付锁定构建中该项被强制开启且不可关闭（见 deliveryNotice），显示一个
                        // 用户改不动的开关只会造成困惑，故隐藏。
                        visible: _swarm.aiAutodispatchAvailable
                        text:    qsTr("所有 AI 动作均需确认")
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
                // 交付锁定构建中自主等级被固定在 L2 且无法更改（见 deliveryNotice），下拉框和
                // 说明文字对操作员没有实际意义，一并隐藏；标题一起隐藏以免留下空标题。
                QGCLabel {
                    text:       qsTr("◆ 自主等级")
                    font.bold:  true
                    visible:    _swarm.aiAutodispatchAvailable
                }
                RowLayout {
                    width:      parent.width
                    spacing: _margin
                    visible:    _swarm.aiAutodispatchAvailable
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
                    // 锁定构建里 autonomyLevel 恒为 2，本条件本就恒假；显式并上
                    // aiAutodispatchAvailable，避免日后有人改动 autonomyLevel 的取值范围时
                    // 意外把这个编辑器重新亮出来。
                    visible:            _swarm.autonomyLevel === 3 && _swarm.aiAutodispatchAvailable
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

                QGCLabel {
                    width: parent.width
                    visible: !_root._swarm.constraintsValid
                    text: qsTr("约束配置无效：%1。地图保留上次有效围栏；起飞、航线启动和编队受限。").arg(_root._swarm.constraintsError)
                    color: "#ffb74d"
                    wrapMode: Text.Wrap
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
            QGCTextField {
                id:                 inputField
                objectName:         "swarmAIInputField"
                width:              parent.width
                placeholderText:    qsTr("输入集群任务，如：全体起飞到30米后楔形编队")
                enabled:            !_ai.busy
                onAccepted: {
                    if (sendButton.enabled) {
                        sendButton.clicked()
                    }
                }
            }
            RowLayout {
                width:   parent.width
                spacing: _margin
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
                Item { Layout.fillWidth: true }
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

                    // Keep wrapping labels and editors inside the available panel width.
                    property real _planColumnMaxItemWidth: _root.width - _margin * 4

                    // 中止请求没有动作列表，下方 planSummary/pendingTaskModel 两个 Repeater 都为空；
                    // 不单独说明的话操作员会看到一个只有两个按钮的空确认框。
                    QGCLabel {
                        visible:    _ai.pendingAbort
                        text:       qsTr("⚠ 模型请求中止当前任务。确认后全体停止自动动作并悬停。")
                        font.bold:  true
                        color:      "#ff8080"
                        wrapMode:   Text.Wrap
                        Layout.fillWidth: true
                        Layout.maximumWidth: planColumn._planColumnMaxItemWidth
                    }
                    QGCLabel {
                        visible:    !_ai.pendingAbort
                        text:       _ai.planHasDangerous ? qsTr("⚠ AI 行动计划（含危险动作，请确认）")
                                                         : qsTr("AI 行动计划")
                        font.bold:  true
                        color:      _ai.planHasDangerous ? "#ff8080" : qgcPal.text
                        wrapMode:   Text.Wrap
                        Layout.fillWidth: true
                        // The dangerous-plan variant of this text alone measured 136px unwrapped --
                        // 1px under swarmAIPanelFlickable's 138px clip width with every other row in
                        // this file capped, which is not a safe margin. Same cap, same reason as
                        // planColumn._planColumnMaxItemWidth.
                        Layout.maximumWidth: planColumn._planColumnMaxItemWidth
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
                                // See planColumn._planColumnMaxItemWidth: this line's own text was
                                // measured forcing planColumn's implicitWidth to 290px (available
                                // ~132px) before this cap was added -- the original overflow's actual
                                // root cause, not the fields below.
                                Layout.maximumWidth: planColumn._planColumnMaxItemWidth
                            }
                            RowLayout {
                                visible:    editParamsToggle.checked
                                spacing:    _margin / 2
                                QGCTextField {
                                    id:                 paramField
                                    Layout.fillWidth:   true
                                    // Floor: never squeezed unusably narrow. Ceiling: never the
                                    // element that inflates the shared column (see
                                    // planColumn._planColumnMaxItemWidth) even with long JSON content.
                                    Layout.minimumWidth: ScreenTools.defaultFontPixelWidth * 8
                                    Layout.maximumWidth: planColumn._planColumnMaxItemWidth
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
                    // ==================== 待确认任务：逐阶段编辑 ====================
                    // 注意（不可忽略的已知限制）：下面任一「应用」按钮成功提交都会触发
                    // planChanged()，其会重新读取 pendingTaskModel() 并整体重建本 Repeater
                    // 的子树。这意味着一次提交会把本阶段乃至其他阶段里所有“已输入但尚未应用”
                    // 的文本框内容一并冲回上一次已提交的值，且没有任何提示——逐字段单独编辑、
                    // 单独应用没问题，同时改动多个字段再统一应用会静默丢字。跨刷新保留未提交
                    // 草稿需要额外的状态管理，超出本次改动范围，先在此记录以免日后被重新踩坑。
                    Repeater {
                        model: _ai.pendingTaskModel
                        delegate: ColumnLayout {
                            id:                       stageDelegate
                            required property var     modelData
                            required property int     index
                            // 阶段序号显式具名保存：内层 Repeater 自己的 required property int
                            // index 会遮蔽这里的 index，直接用 index 或 parent.parent.index 都
                            // 会拿到错误的（或随布局改动就会碎掉的）值，因此用带 id 限定的
                            // stageDelegate.stageIndex 让内层显式引用外层这一份。
                            readonly property int     stageIndex: index
                            Layout.fillWidth: true
                            spacing: ScreenTools.defaultFontPixelHeight / 4

                            QGCLabel {
                                text:       qsTr("阶段 %1「%2」").arg(stageDelegate.stageIndex + 1).arg(stageDelegate.modelData.name)
                                font.bold:  true
                                wrapMode:   Text.Wrap
                                Layout.fillWidth: true
                                // Stage names come from the AI's own task_name/name fields, with no
                                // length limit -- capped for the same reason as the flat-plan
                                // summary label above (see planColumn._planColumnMaxItemWidth).
                                Layout.maximumWidth: planColumn._planColumnMaxItemWidth
                            }

                            // 面板固定宽度仅约 24 个字号单位：标签、输入框、按钮三者各自独占一行。
                            // 曾经的做法是标签单独一行、输入框与按钮共享下一行（同文件内 flat-plan
                            // 编辑器 :373-391 仍是这样），改成三者各占一行是为了不让同一行里的两样
                            // 东西互相挤占。但这不是 Task 7c 那次点不动的根因：实测 planColumn 的
                            // implicitWidth 被撑到 290px（可用列宽仅 132px）；撑大它的是本文件内两处
                            // 带 wrapMode: Text.Wrap、没有宽度上限的 QGCLabel（flat-plan 编辑器的摘要
                            // 行、下方完成条件提示行）——Text 在没有宽度约束时 implicitWidth 报告的是
                            // 整行不换行的自然宽度，而单列 ColumnLayout 会用这个未加限制的宽度覆盖
                            // anchors.fill 给出的 ~132px，把同一 planColumn 里所有行（包括这里的输入
                            // 框）一并撑宽、撑出 swarmAIPanelFlickable 的可视区。经实测验证，起作用的
                            // 是 Layout.maximumWidth（加宽度上限），不是最初设想的 Layout.minimumWidth
                            // ——minimumWidth 加了也测不出任何变化。真正的修复是给所有会换行的 QGCLabel
                            // 和这几个输入框统一加 Layout.maximumWidth: planColumn._planColumnMaxItemWidth
                            // （见该属性定义处的完整说明），minimumWidth 保留作为下限防御。详见 Task 7c 报告。
                            Repeater {
                                model: stageDelegate.modelData.actions
                                delegate: ColumnLayout {
                                    required property var modelData
                                    required property int index
                                    Layout.fillWidth: true
                                    spacing: 0

                                    QGCLabel {
                                        text:       qsTr("动作 %1 %2").arg(index + 1).arg(modelData.type)
                                        wrapMode:   Text.Wrap
                                        Layout.fillWidth: true
                                        Layout.maximumWidth: planColumn._planColumnMaxItemWidth
                                    }
                                    QGCTextField {
                                        id:               actionParamField
                                        objectName:       "swarmStageActionField"
                                        visible:          editParamsToggle.checked
                                        text:             modelData.paramsJson
                                        enabled:          modelData.editable
                                        placeholderText:  qsTr("{\"altitude\": 40}")
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: ScreenTools.defaultFontPixelWidth * 8
                                        Layout.maximumWidth: planColumn._planColumnMaxItemWidth
                                    }
                                    QGCButton {
                                        objectName: "swarmStageActionApply"
                                        visible:    editParamsToggle.checked
                                        text:       modelData.editable ? qsTr("应用") : qsTr("不可改")
                                        enabled:    modelData.editable
                                        Layout.alignment: Qt.AlignRight
                                        onClicked: _ai.updatePendingStageAction(stageDelegate.stageIndex, index,
                                                                                actionParamField.text)
                                    }
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                QGCLabel {
                                    // "可加入 kind 切换条件类型" 提示放在这里而非输入框占位符：这一行
                                    // 换行、占满整列宽度，放得下；占位符只有一行且字段本身要给参数值
                                    // 留出可读空间，塞不下这句话，硬塞只会撑大隐式宽度、显示不出来。
                                    text:       qsTr("完成条件 %1（可加入 \"kind\" 切换条件类型）").arg(stageDelegate.modelData.conditionKind)
                                    wrapMode:   Text.Wrap
                                    Layout.fillWidth: true
                                    // Measured 217px unwrapped for conditionKind "all_airborne" -- the
                                    // second of the two labels that were actually inflating planColumn
                                    // (see planColumn._planColumnMaxItemWidth).
                                    Layout.maximumWidth: planColumn._planColumnMaxItemWidth
                                }
                                QGCTextField {
                                    id:               conditionField
                                    objectName:       "swarmStageConditionField"
                                    visible:          editParamsToggle.checked
                                    text:             stageDelegate.modelData.conditionParamsJson
                                    placeholderText:  qsTr("{\"timeout_sec\": 600}")
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: ScreenTools.defaultFontPixelWidth * 8
                                    Layout.maximumWidth: planColumn._planColumnMaxItemWidth
                                }
                                QGCButton {
                                    objectName: "swarmStageConditionApply"
                                    visible: editParamsToggle.checked
                                    text: qsTr("应用")
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: {
                                        var payload = conditionField.text
                                        var edited = null
                                        try {
                                            edited = JSON.parse(payload)
                                        } catch (e) {
                                            edited = null
                                        }
                                        // 参数框里没写 kind 时补回当前 kind；写了就尊重操作员的选择，
                                        // 否则改成 manual 这类换档操作永远做不到。
                                        if (edited !== null && edited.kind === undefined) {
                                            edited.kind = stageDelegate.modelData.conditionKind
                                            payload = JSON.stringify(edited)
                                        }
                                        // 解析失败时原样下送，由 C++ 侧既有守卫统一给出
                                        // “完成条件 JSON 解析失败，请检查格式”，不在 QML 里另开一条提示通道。
                                        _ai.updatePendingStageCondition(stageDelegate.stageIndex, payload)
                                    }
                                }
                            }
                        }
                    }

                    // Split across two rows rather than one three-item RowLayout: the combined
                    // implicit width of 确认执行 + 放弃 + 编辑参数 (~157px) measured as the residual
                    // forcing element once the two QGCLabels above were capped -- planColumn's only
                    // available width is ~132px, so all three never fit on one line regardless of
                    // Layout.fillWidth (which only distributes *extra* space, not a shortfall). Two
                    // rows keep every control at its full, untruncated size instead of squeezing or
                    // clipping button text. See planColumn._planColumnMaxItemWidth.
                    RowLayout {
                        spacing: _margin
                        QGCButton { text: qsTr("确认执行"); onClicked: _ai.confirmPlan() }
                        QGCButton { text: qsTr("放弃");     onClicked: _ai.rejectPlan() }
                    }
                    QGCCheckBox {
                        id:         editParamsToggle
                        objectName: "swarmEditParamsToggle"
                        text:       qsTr("编辑参数")
                        checked:    false
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

            QGCLabel {
                width: parent.width
                wrapMode: Text.Wrap
                visible: _root._swarm.executionMessage.length > 0
                text: _root._swarm.executionMessage
            }
            Repeater {
                model: Object.keys(_root._swarm.executionStates)
                delegate: QGCLabel {
                    required property string modelData
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: {
                        const labels = { pending: qsTr("等待下发"), dispatching: qsTr("等待响应"),
                            accepted: qsTr("动作已确认"), failed: qsTr("动作失败"),
                            withdrawn: qsTr("已退出"), cancelled: qsTr("已取消") }
                        const state = _root._swarm.executionStates[modelData]
                        return qsTr("%1 号机：%2").arg(modelData).arg(labels[state] || state)
                    }
                }
            }
            Repeater {
                model: Object.keys(_root._swarm.missionPlanner.uploadStates)
                delegate: QGCLabel {
                    required property string modelData
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: {
                        const labels = { uploading: qsTr("上传中"), reading_back: qsTr("回读核验中"),
                            verified: qsTr("航线核验通过"), failed: qsTr("航线上传失败"), cancelled: qsTr("上传已取消") }
                        const state = _root._swarm.missionPlanner.uploadStates[modelData]
                        return qsTr("%1 号机：%2").arg(modelData).arg(labels[state] || state)
                    }
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
                    QGCLabel {
                        visible: _root._task.persistenceError.length > 0
                        text: _root._task.persistenceError
                        color: "#ffb74d"
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
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
                            text: qsTr("确认机载任务已完成")
                            visible: _root._task.awaitingMissionConfirmation
                            onClicked: {
                                completionConfirm.token = _root._task.executionToken
                                completionConfirm.open()
                            }
                        }
                        QGCButton {
                            text:       qsTr("继续（人工确认）")
                            visible:    _task.waitingForProceed
                            onClicked:  _task.proceed()
                        }
                        QGCButton {
                            text:       qsTr("重试当前阶段")
                            visible:    _task.canRetry
                            onClicked:  _task.retryBlockedStage()
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
                                id: parameterRow
                                required property var modelData
                                spacing: _margin
                                QGCLabel {
                                    text: parameterRow.modelData.label
                                    Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 9
                                    elide: Text.ElideRight
                                }
                                QGCTextField {
                                    Layout.fillWidth: true
                                    readonly property bool automaticDefault: ["climb_gate", "leader_id", "group_a_ids", "group_b_ids"].indexOf(parameterRow.modelData.key) >= 0
                                    text: automaticDefault ? "" : (parameterRow.modelData.type === "array" ? JSON.stringify(parameterRow.modelData.default) : parameterRow.modelData.default)
                                    placeholderText: automaticDefault ? qsTr("自动匹配任务参数与成员") : ""
                                    onEditingFinished: {
                                        if (text.trim().length === 0)
                                            delete tplDelegate._paramValues[parameterRow.modelData.key]
                                        else
                                            tplDelegate._paramValues[parameterRow.modelData.key] = text
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    MessageDialog {
        id: completionConfirm
        property string token: ""
        title: qsTr("确认任务完成")
        text: qsTr("请核对所有剩余飞机的机载任务已完成。确认后将执行下一阶段。")
        buttons: MessageDialog.Yes | MessageDialog.No
        onAccepted: _root._task.confirmMissionCompletion(completionConfirm.token)
    }
}
