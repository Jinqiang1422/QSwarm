#include "SwarmMissionPlanner.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QtMath>
#include <algorithm>
#include <cmath>
#include <utility>

#include "FirmwarePlugin.h"
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
    // PlanManager consumes the Home placeholder according to FirmwarePlugin policy.
    items.append(new MissionItem(seq++, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL, 0, 0, 0, 0, homeCoord.latitude(),
                                 homeCoord.longitude(), homeCoord.altitude(), true, false));
    items.append(new MissionItem(seq++, MAV_CMD_NAV_TAKEOFF, MAV_FRAME_GLOBAL_RELATIVE_ALT_INT, 0, 0, 0, 0,
                                 homeCoord.latitude(), homeCoord.longitude(), _surveyAltitude, true /*autoContinue*/,
                                 true /*isCurrentItem*/));
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
    if (busy() || !center.isValid() || !std::isfinite(lengthMeters) || !std::isfinite(widthMeters) ||
        !std::isfinite(angleDeg) || !std::isfinite(_surveyAltitude) || !std::isfinite(_surveySpacing) ||
        lengthMeters < 10.0 || lengthMeters > 100000.0 || widthMeters < 10.0 || widthMeters > 100000.0 ||
        _surveyAltitude < 3.0 || _surveyAltitude > 120.0 || _surveySpacing < 2.0) {
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
    // MISSION_COUNT and sequence fields are uint16; leave room for Home, takeoff and RTL.
    if (std::ceil(widthMeters / vehicleCount / _surveySpacing) * 2 + 3 > 65535) {
        emit plannerFeedback(tr("任务规划失败：航点数量超过协议上限，请增大行距或缩小区域"));
        return false;
    }

    for (Vehicle* vehicle : targets) {
        if (!vehicle || !vehicle->missionManager() || vehicle->missionManager()->inProgress()) {
            emit plannerFeedback(tr("任务规划失败：目标任务管理器忙碌"));
            return false;
        }
    }
    ++_batchId;
    _batchFailed = false;
    _verifiedCount = 0;
    _verifiedRevisions.clear();
    _uploadStates.clear();

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
    QList<QList<MissionItem*>> missions;

    for (int v = 0; v < vehicleCount; v++) {
        const double bandMin = -widthMeters / 2.0 + bandOfVehicle[v] * bandWidth;
        const double bandMax = bandMin + bandWidth;
        const QGeoCoordinate homeCoord = targets[v]->coordinate().isValid() ? targets[v]->coordinate() : center;

        QList<MissionItem*> items =
            buildCoverageMissionItems(homeCoord, center, lengthMeters, widthMeters, angleDeg, bandMin, bandMax);
        totalWaypoints += items.count() - 3;  // 去掉起飞/返航项

        // 与人工 Plan 视图“上传”相同的通道：PlanManager::writeMissionItems
        // （QGC MissionController 上传人工任务时调用的正是此函数）
        missions.append(items);
    }
    // Register every member before any synchronous rejection can complete the batch.
    for (int v = 0; v < vehicleCount; ++v)
        _trackUpload(targets[v], missions[v]);
    for (int v = 0; v < vehicleCount; ++v) {
        if (_batchFailed || !targets[v] || targets[v]->missionManager()->inProgress()) {
            qDeleteAll(missions[v]);
            if (targets[v])
                _finishUpload(targets[v]->id(), false, tr("上传批次未能完整派发"));
        } else {
            targets[v]->missionManager()->writeMissionItems(missions[v]);
        }
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

namespace {
QList<QList<double>> missionSignature(const QList<MissionItem*>& items, bool hasHome)
{
    QList<QList<double>> result;
    for (int i = hasHome ? 1 : 0; i < items.size(); ++i) {
        const MissionItem* item = items[i];
        if (!item)
            return {};
        int frame = item->frame();
        if (frame == MAV_FRAME_GLOBAL_INT)
            frame = MAV_FRAME_GLOBAL;
        if (frame == MAV_FRAME_GLOBAL_RELATIVE_ALT_INT)
            frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
        result.append({double(item->command()), double(frame), item->param1(), item->param2(), item->param3(),
                       item->param4(), item->param5(), item->param6(), item->param7(), double(item->autoContinue())});
    }
    return result;
}

bool sameMission(const QList<QList<double>>& expected, const QList<QList<double>>& actual)
{
    if (expected.size() != actual.size() || expected.isEmpty())
        return false;
    for (int i = 0; i < expected.size(); ++i) {
        if (expected[i].size() != actual[i].size())
            return false;
        for (int p = 0; p < expected[i].size(); ++p) {
            const double a = expected[i][p], b = actual[i][p];
            if (std::isnan(a) && std::isnan(b))
                continue;
            const double tolerance = (p == 6 || p == 7) ? 1.1e-7 : 1e-4;
            if (!std::isfinite(a) || !std::isfinite(b) || std::abs(a - b) > tolerance)
                return false;
        }
    }
    return true;
}
}  // namespace

void SwarmMissionPlanner::_trackUpload(Vehicle* vehicle, const QList<MissionItem*>& items)
{
    if (!vehicle || !vehicle->missionManager())
        return;
    const int id = vehicle->id();
    auto* context = new QObject(this);
    _uploads.insert(id, context);
    _uploadStates[QString::number(id)] = QStringLiteral("uploading");
    _missionStatus.remove(QString::number(id));
    emit missionStatusChanged();
    const auto expected = missionSignature(items, true);
    QPointer<Vehicle> target(vehicle);
    QPointer<MissionManager> manager(vehicle->missionManager());
    const bool sendsHome = vehicle->firmwarePlugin()->sendHomePositionToVehicle();
    connect(vehicle, &QObject::destroyed, context, [this, id] { _finishUpload(id, false, tr("载具断开")); });
    connect(manager, &MissionManager::error, context,
            [this, id](int, const QString& reason) { _finishUpload(id, false, reason); });
    connect(
        manager, &MissionManager::sendComplete, context,
        [this, id, manager, target, context, expected, sendsHome](bool error) {
            if (error || !manager || !target) {
                _finishUpload(id, false, tr("上传失败"));
                return;
            }
            _uploadStates[QString::number(id)] = QStringLiteral("reading_back");
            emit missionStatusChanged();
            connect(
                manager, &MissionManager::newMissionItemsAvailable, context,
                [this, id, manager, expected, sendsHome](bool removed) {
                    if (!manager || removed ||
                        !sameMission(expected, missionSignature(manager->missionItems(), sendsHome))) {
                        _finishUpload(id, false, tr("机载任务回读与上传内容不一致"));
                        return;
                    }
                    const int count = manager->missionItems().size();
                    _verifiedRevisions[id] = manager->executionRevision();
                    _missionStatus[QString::number(id)] = count;
                    emit missionUploaded(id, count);
                    _finishUpload(id, true, {});
                },
                Qt::SingleShotConnection);
            manager->loadFromVehicle();
        },
        Qt::SingleShotConnection);
    QTimer::singleShot(120000, context, [this, id] { _finishUpload(id, false, tr("上传/回读超时")); });
}

void SwarmMissionPlanner::_finishUpload(int vehicleId, bool success, const QString& reason, bool withdrawn)
{
    QObject* context = _uploads.take(vehicleId);
    if (!context)
        return;
    // Disconnect immediately: deleteLater alone still permits signals from the terminating transaction.
    context->disconnect();
    delete context;
    _batchFailed |= !success && !withdrawn;
    if (success)
        ++_verifiedCount;
    _uploadStates[QString::number(vehicleId)] = withdrawn ? QStringLiteral("cancelled")
                                                : success ? QStringLiteral("verified")
                                                          : QStringLiteral("failed");
    emit missionStatusChanged();
    if (!reason.isEmpty())
        emit plannerFeedback(tr("%1 号机：%2").arg(vehicleId).arg(reason));
    if (_uploads.isEmpty())
        emit batchFinished(_batchId, !_batchFailed && _verifiedCount > 0,
                           _verifiedCount == 0 && !_batchFailed ? tr("全部上传成员已退出") : reason);
}

void SwarmMissionPlanner::cancelTargets(const QList<int>& ids)
{
    for (int id : ids)
        _finishUpload(id, false, {}, true);
}

void SwarmMissionPlanner::clearAllMissions()
{
    cancelTargets(_uploads.keys());
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
