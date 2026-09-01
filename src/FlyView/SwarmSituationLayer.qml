/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmSituationLayer（WP3.4 共同态势图）
 *
 * 地图态势叠加层（挂在 FlyView 地图的 Map 组件内，见集成补丁说明 §P3）：
 *   - 目标航迹：类别图标 + 置信度环 + 编号/速度标签（≥1Hz 刷新）
 *   - 威胁区/禁飞区：红色半透明多边形（fences.json exclude）
 *   - 搜索覆盖热力：琥珀色格网（权重越深越熟，回答“哪片还没搜过”）
 *
 * 数据源：SwarmManager.copModel（QAbstractListModel，MapItemView 直接绑定）
 *
 ****************************************************************************/

import QtQuick
import QtLocation
import QtPositioning

import QGroundControl
import QGroundControl.Controls

Item {
    id: _layer
    objectName: "swarmSituationLayer"

    /// 外部注入：宿主 Map 与 SwarmManager（集成补丁在 FlyView 地图处绑定）
    property var map:           null
    property var swarmManager:  null
    property bool showHeatmap:  true
    property bool showThreats:  true
    property bool showTracks:   true

    // ----------------------------------------------------------------------
    // 威胁区/禁飞区多边形
    // ----------------------------------------------------------------------
    MapItemView {
        parent: _layer.map
        model:  (_layer.showThreats && _layer.swarmManager) ? _layer.swarmManager.copModel.threatZones : []
        delegate: MapPolygon {
            objectName: "swarmThreatZone"
            color:        "#40e53935"
            border.color: "#e53935"
            border.width: 2
            path: {
                var pts = []
                for (var i = 0; i < modelData.length; i++) {
                    pts.push(QtPositioning.coordinate(modelData[i][0], modelData[i][1]))
                }
                return pts
            }
        }
    }

    // ----------------------------------------------------------------------
    // 搜索覆盖热力格网（~55m 格，权重 1~10 映射透明度）
    // ----------------------------------------------------------------------
    MapItemView {
        parent: _layer.map
        model:  (_layer.showHeatmap && _layer.swarmManager) ? _layer.swarmManager.copModel.heatmap : []
        delegate: MapRectangle {
            objectName: "swarmHeatmapCell"
            topLeft:     QtPositioning.coordinate(modelData.lat + 0.00025, modelData.lon - 0.00025)
            bottomRight: QtPositioning.coordinate(modelData.lat - 0.00025, modelData.lon + 0.00025)
            color:       Qt.rgba(1.0, 0.67, 0.0, 0.05 + 0.03 * modelData.weight)
            border.width: 0
        }
    }

    // ----------------------------------------------------------------------
    // 目标航迹标记
    // ----------------------------------------------------------------------
    MapItemView {
        parent: _layer.map
        model:  (_layer.showTracks && _layer.swarmManager) ? _layer.swarmManager.copModel : null
        delegate: MapQuickItem {
            objectName:      "swarmTrackMarker_" + model.fusedId
            coordinate:      model.coordinate
            anchorPoint.x:   marker.width / 2
            anchorPoint.y:   marker.height / 2
            zoomLevel:       0

            sourceItem: Item {
                id: marker
                width:  44
                height: 56

                // 置信度环（越低越虚）
                Rectangle {
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: -8
                    width: 34; height: 34
                    radius: 17
                    color: "transparent"
                    border.width: 2
                    border.color: model.trackState === "coasting" ? "#90A4AE" : "#4fc3f7"
                    opacity: 0.35 + 0.6 * model.confidence
                }
                // 类别圆点
                Rectangle {
                    id: dot
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: -8
                    width: 22; height: 22
                    radius: 11
                    color: {
                        switch (model.category) {
                        case 1:  return "#8e24aa"     // 人员 紫
                        case 2:  return "#e53935"     // 车辆 红
                        case 3:  return "#1e88e5"     // 船只 蓝
                        case 4:  return "#43a047"     // 动物 绿
                        case 5:  return "#fb8c00"     // 火点 橙
                        case 6:  return "#757575"     // 烟雾 灰
                        default: return "#546e7a"
                        }
                    }
                    QGCLabel {
                        anchors.centerIn: parent
                        text:           model.categoryName.charAt(0)
                        color:          "white"
                        font.pointSize: ScreenTools.smallFontPointSize
                    }
                }
                // 编号 + 速度标签
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom:           parent.bottom
                    width:  infoLabel.width + 6
                    height: infoLabel.height + 2
                    radius: 3
                    color:  "#bb000000"
                    QGCLabel {
                        id: infoLabel
                        anchors.centerIn: parent
                        text:  "#" + model.fusedId + " " + model.speed.toFixed(1) + "m/s"
                        color: "white"
                        font.pointSize: ScreenTools.smallFontPointSize
                    }
                }
            }
        }
    }
}
