#include "SwarmMissionPlanner.h"

#include <QtCore/QPointer>
#include <QtCore/QtMath>
#include <algorithm>
#include <utility>

#include "MissionItem.h"
#include "MissionManager.h"
#include "MultiVehicleManager.h"
#include "QGCApplication.h"
#include "QmlObjectListModel.h"
#include "SwarmTaskAllocator.h"
#include "SwarmTypes.h"
#include "Vehicle.h"

SwarmMissionPlanner::SwarmMissionPlanner(QObject* parent) : QObject(parent)
{
    (void) connect(this, &SwarmMissionPlanner::plannerFeedback, qgcApp(),
                   [](const QString& message) { qgcApp()->showAppMessage(message); });
}

SwarmMissionPlanner::~SwarmMissionPlanner() = default;

void SwarmMissionPlanner::setVehicleAutomationAllowedCallback(std::function<bool(int)> callback)
{
    _vehicleAutomationAllowed = std::move(callback);
}

void SwarmMissionPlanner::setSurveyAltitude(double altitude)
{
    if (!qFuzzyCompare(_surveyAltitude, altitude)) {
        _surveyAltitude = qMax(3.0, altitude);
        emit surveyAltitudeChanged();
    }
}

void SwarmMissionPlanner::setSurveySpacing(double spacing)
{
    if (!qFuzzyCompare(_surveySpacing, spacing)) {
        _surveySpacing = qMax(2.0, spacing);
        emit surveySpacingChanged();
    }
}

QGeoCoordinate SwarmMissionPlanner::_localToGeo(const QGeoCoordinate& center, double angleDeg, double x, double y) const
{
    // 机位本地系：x 沿航线方向(方位角 angleDeg)，y 垂直航线方向(方位角 angleDeg+90)
    const double a = qDegreesToRadians(angleDeg);
    const double east = x * qSin(a) + y * qCos(a);
    const double north = x * qCos(a) - y * qSin(a);
    const double distance = qSqrt(east * east + north * north);
    const double azimuth = qRadiansToDegrees(qAtan2(east, north));
    return center.atDistanceAndAzimuth(distance, azimuth);
}

// ---------------------------------------------------------------------------
// 任务项构建（与上传分离；使用与 QGC Plan 视图相同的 MissionItem 原语）
// ---------------------------------------------------------------------------
QList<MissionItem*> SwarmMissionPlanner::buildCoverageMissionItems(const QGeoCoordinate& homeCoord,
                                                                   const QGeoCoordinate& center, double lengthMeters,
                                                                   double widthMeters, double angleDeg, double bandMin,
                                                                   double bandMax) const
{
    Q_UNUSED(widthMeters);  // 条带宽度已体现在 bandMin/bandMax 中，此处按条带生成航点无需整体宽度
    // 条带内生成割草机航线：行位于条带内均布，方向逐行交替
    QList<QGeoCoordinate> waypoints;
    const double bandWidth = bandMax - bandMin;
    const int rowCount = qMax(1, qCeil(bandWidth / _surveySpacing));
    for (int row = 0; row < rowCount; row++) {
        const double y = bandMin + (row + 0.5) * (bandWidth / rowCount);
        const bool forward = (row % 2 == 0);
        const double x1 = forward ? -lengthMeters / 2.0 : lengthMeters / 2.0;
        const double x2 = forward ? lengthMeters / 2.0 : -lengthMeters / 2.0;
        waypoints.append(_localToGeo(center, angleDeg, x1, y));
        waypoints.append(_localToGeo(center, angleDeg, x2, y));
    }

    // 组装任务项：起飞 → 航点 → 返航（与 MissionController 产出相同的序列结构）
    QList<MissionItem*> items;
    int seq = 0;
    items.append(new MissionItem(seq++, MAV_CMD_NAV_TAKEOFF, MAV_FRAME_GLOBAL_RELATIVE_ALT_INT, 0, 0, 0, 0,
                                 homeCoord.latitude(), homeCoord.longitude(), _surveyAltitude, true /*autoContinue*/,
                                 false /*isCurrentItem*/));
    for (const QGeoCoordinate& wp : waypoints) {
        items.append(new MissionItem(seq++, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT_INT, 0 /*hold*/,
                                     0 /*acceptRadius 默认*/, 0 /*passRadius*/, 0 /*yaw*/, wp.latitude(),
                                     wp.longitude(), _surveyAltitude, true, false));
    }
    items.append(
        new MissionItem(seq++, MAV_CMD_NAV_RETURN_TO_LAUNCH, MAV_FRAME_MISSION, 0, 0, 0, 0, 0, 0, 0, true, false));
    return items;
}

bool SwarmMissionPlanner::planAndUploadCoverage(const QGeoCoordinate& center, double lengthMeters, double widthMeters,
                                                double angleDeg, const QString& assignMode)
{
    if (!center.isValid() || lengthMeters < 10.0 || widthMeters < 10.0) {
        emit plannerFeedback(tr("任务规划失败：区域参数无效"));
        return false;
    }

    // Manager 的回调绑定了计划目标；存在回调时不再读取可能已变化的 UI 选择。
    MultiVehicleManager* mvm = MultiVehicleManager::instance();
    QmlObjectListModel* model =
        _vehicleAutomationAllowed
            ? mvm->vehicles()
            : ((mvm->selectedVehicles()->count() > 0) ? mvm->selectedVehicles() : mvm->vehicles());
    QList<Vehicle*> targets;
    for (int i = 0; i < model->count(); i++) {
        Vehicle* vehicle = qobject_cast<Vehicle*>(model->get(i));
        if (vehicle && (!_vehicleAutomationAllowed || _vehicleAutomationAllowed(vehicle->id()))) {
            targets.append(vehicle);
        }
    }
    std::sort(targets.begin(), targets.end(), [](const Vehicle* a, const Vehicle* b) { return a->id() < b->id(); });

    const int vehicleCount = targets.count();
    if (vehicleCount == 0) {
        emit plannerFeedback(tr("任务规划失败：没有在线无人机"));
        return false;
    }

    // 沿垂直航线方向把区域均匀切分为 vehicleCount 个条带
    const double bandWidth = widthMeters / vehicleCount;

    // WP2.4：条带→无人机分配。默认按 id 升序；auction 模式用拍卖法最小化总转场里程
    QList<int> bandOfVehicle(vehicleCount);
    for (int v = 0; v < vehicleCount; v++) {
        bandOfVehicle[v] = v;
    }
    if ((assignMode == QStringLiteral("auction") || assignMode == QStringLiteral("cbba")) && vehicleCount > 1) {
        SwarmTaskAllocator allocator;
        QList<SwarmTaskAllocator::Node> vehicleNodes, bandNodes;
        for (int v = 0; v < vehicleCount; v++) {
            const QGeoCoordinate vc = targets[v]->coordinate().isValid() ? targets[v]->coordinate() : center;
            double eastM, northM;
            SwarmTypes::latLonToLocalM(center.latitude(), center.longitude(), vc.latitude(), vc.longitude(), eastM,
                                       northM);
            vehicleNodes.append({targets[v]->id(), eastM, northM});
        }
        for (int b = 0; b < vehicleCount; b++) {
            const double yMid = -widthMeters / 2.0 + (b + 0.5) * bandWidth;
            const QGeoCoordinate bandCenter = _localToGeo(center, angleDeg, 0.0, yMid);
            double eastM, northM;
            SwarmTypes::latLonToLocalM(center.latitude(), center.longitude(), bandCenter.latitude(),
                                       bandCenter.longitude(), eastM, northM);
            bandNodes.append({b, eastM, northM});  // 条带 Node id = 条带索引
        }
        const bool useCbba = (assignMode == QStringLiteral("cbba"));
        const QMap<int, int> assignment = useCbba
                                              ? allocator.cbbaAssign(vehicleNodes, bandNodes)      // 分布式一致性拍卖
                                              : allocator.auctionAssign(vehicleNodes, bandNodes);  // 集中式拍卖(最优)
        const QVariantMap cmp = allocator.compare(vehicleNodes, bandNodes);
        for (int v = 0; v < vehicleCount; v++) {
            bandOfVehicle[v] = assignment.value(targets[v]->id(), v);
        }
        const double seqTotal = cmp.value(QStringLiteral("seq_total_m")).toDouble();
        const double optTotal =
            cmp.value(useCbba ? QStringLiteral("cbba_total_m") : QStringLiteral("auction_total_m")).toDouble();
        if (seqTotal > 1.0) {
            emit plannerFeedback(tr("%1条带分配：总转场里程 %2 米（顺序分配 %3 米，降低 %4%）")
                                     .arg(useCbba ? tr("CBBA 分布式") : tr("拍卖法"))
                                     .arg(optTotal, 0, 'f', 0)
                                     .arg(seqTotal, 0, 'f', 0)
                                     .arg((seqTotal - optTotal) / seqTotal * 100.0, 0, 'f', 1));
        }
    }

    int totalWaypoints = 0;

    for (int v = 0; v < vehicleCount; v++) {
        const double bandMin = -widthMeters / 2.0 + bandOfVehicle[v] * bandWidth;
        const double bandMax = bandMin + bandWidth;
        const QGeoCoordinate homeCoord = targets[v]->coordinate().isValid() ? targets[v]->coordinate() : center;

        QList<MissionItem*> items =
            buildCoverageMissionItems(homeCoord, center, lengthMeters, widthMeters, angleDeg, bandMin, bandMax);
        totalWaypoints += items.count() - 2;  // 去掉起飞/返航项

        // 与人工 Plan 视图“上传”相同的通道：PlanManager::writeMissionItems
        // （QGC MissionController 上传人工任务时调用的正是此函数）
        _trackUpload(targets[v], items.count());
        targets[v]->missionManager()->writeMissionItems(items);
    }

    _lastPlanVehicleCount = vehicleCount;
    _lastPlanWaypointCount = totalWaypoints;
    emit planChanged();
    emit plannerFeedback(tr("区域覆盖任务已提交上传请求：%1 架无人机，共 %2 个航点（条带宽度 %3 米）")
                             .arg(vehicleCount)
                             .arg(totalWaypoints)
                             .arg(bandWidth, 0, 'f', 1));
    return true;
}

void SwarmMissionPlanner::_trackUpload(Vehicle* vehicle, int itemCount)
{
    // 上传完成后回读同步：地面站任务缓存与机载保持一致，
    // QGC Plan 视图即可显示并允许人工继续编辑这份 AI 生成的任务（同一套任务）。
    QPointer<Vehicle> vehiclePtr(vehicle);
    QPointer<MissionManager> mmPtr(vehicle->missionManager());
    connect(
        vehicle->missionManager(), &MissionManager::sendComplete, this,
        [this, vehiclePtr, mmPtr, itemCount](bool error) {
            if (error || !mmPtr || !vehiclePtr) {
                return;
            }
            mmPtr->loadFromVehicle();
            _missionStatus[QString::number(vehiclePtr->id())] = itemCount;
            emit missionStatusChanged();
            emit missionUploaded(vehiclePtr->id(), itemCount);
        },
        Qt::SingleShotConnection);
}

void SwarmMissionPlanner::clearAllMissions()
{
    MultiVehicleManager* mvm = MultiVehicleManager::instance();
    QmlObjectListModel* vehicles = mvm->vehicles();
    int clearedCount = 0;
    for (int i = 0; i < vehicles->count(); i++) {
        Vehicle* vehicle = qobject_cast<Vehicle*>(vehicles->get(i));
        if (vehicle && (!_vehicleAutomationAllowed || _vehicleAutomationAllowed(vehicle->id()))) {
            vehicle->missionManager()->removeAll();
            clearedCount++;
        }
    }
    if (clearedCount == 0) {
        emit plannerFeedback(tr("未清空任务：没有持有有效控制租约的无人机"));
        return;
    }
    _missionStatus.clear();
    emit missionStatusChanged();
    emit plannerFeedback(tr("已请求清空 %1 架无人机任务").arg(clearedCount));
}
