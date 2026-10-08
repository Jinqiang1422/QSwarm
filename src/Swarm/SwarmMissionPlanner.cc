#include "SwarmMissionPlanner.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QtMath>
#include <algorithm>
#include <cmath>
#include <utility>

#include "FirmwarePlugin.h"
#include "MissionCommandTree.h"
#include "MissionCommandUIInfo.h"
#include "MissionItem.h"
#include "MissionManager.h"
#include "MultiVehicleManager.h"
#include "QGCApplication.h"
#include "QmlObjectListModel.h"
#include "SwarmTaskAllocator.h"
#include "SwarmTypes.h"
#include "Vehicle.h"

namespace {
/// Yaw (param4) for a flown takeoff/waypoint item, taken from the firmware's mission command definition
/// like a new Plan view item. Where the firmware honours it, NaN leaves the heading to the vehicle: face
/// the next waypoint, and hold the heading on reaching the last one (0 would turn PX4 aircraft north).
/// Firmware that does not store the field (ArduCopter) gets 0, which is also what it reads back.
double headingParam(Vehicle* vehicle, MAV_CMD command)
{
    bool yawShown = false;
    const MissionCommandUIInfo* uiInfo =
        vehicle ? MissionCommandTree::instance()->getUIInfo(vehicle, QGCMAVLink::VehicleClassGeneric, command)
                : nullptr;
    const MissionCmdParamInfo* yaw = uiInfo ? uiInfo->getParamInfo(4, yawShown) : nullptr;
    return (yaw && yawShown && yaw->nanUnchanged()) ? qQNaN() : 0.0;
}
}  // namespace

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
QList<MissionItem*> SwarmMissionPlanner::buildCoverageMissionItems(Vehicle* vehicle, const QGeoCoordinate& homeCoord,
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
    items.append(new MissionItem(seq++, MAV_CMD_NAV_TAKEOFF, MAV_FRAME_GLOBAL_RELATIVE_ALT_INT, 0, 0, 0,
                                 headingParam(vehicle, MAV_CMD_NAV_TAKEOFF), homeCoord.latitude(),
                                 homeCoord.longitude(), _surveyAltitude, true /*autoContinue*/,
                                 true /*isCurrentItem*/));
    const double waypointYaw = headingParam(vehicle, MAV_CMD_NAV_WAYPOINT);
    for (const QGeoCoordinate& wp : waypoints) {
        items.append(new MissionItem(seq++, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT_INT, 0 /*hold*/,
                                     0 /*acceptRadius 默认*/, 0 /*passRadius*/, waypointYaw, wp.latitude(),
                                     wp.longitude(), _surveyAltitude, true, false));
    }
    items.append(
        new MissionItem(seq++, MAV_CMD_NAV_RETURN_TO_LAUNCH, MAV_FRAME_MISSION, 0, 0, 0, 0, 0, 0, 0, true, false));
    return items;
}

bool SwarmMissionPlanner::planAndUploadCoverage(const QGeoCoordinate& center, double lengthMeters, double widthMeters,
                                                double angleDeg, const QString& assignMode,
                                                std::function<bool(int)> allowed)
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
    QList<QPointer<Vehicle>> targets;
    for (int i = 0; i < model->count(); i++) {
        Vehicle* vehicle = qobject_cast<Vehicle*>(model->get(i));
        if (vehicle && (!_vehicleAutomationAllowed || _vehicleAutomationAllowed(vehicle->id()))) {
            targets.append(vehicle);
        }
    }
    std::sort(targets.begin(), targets.end(), [](const auto& a, const auto& b) { return a->id() < b->id(); });

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

    const auto previousBatch = _batchId;
    for (const auto& vehicle : targets) {
        if (!vehicle || !vehicle->missionManager() || vehicle->missionManager()->inProgress() ||
            (allowed && !allowed(vehicle->id())) || !vehicle || busy() || previousBatch != _batchId) {
            emit plannerFeedback(tr("任务规划失败：目标任务管理器忙碌"));
            return false;
        }
    }
    ++_batchId;
    const auto batch = _batchId;
    _batchFailed = false;
    _verifiedCount = 0;
    _verifiedRevisions.clear();
    _verifiedSnapshots.clear();
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
        const auto target = targets[v];
        if (_batchId != batch || !target || (allowed && !allowed(target->id())) || _batchId != batch || !target ||
            target->missionManager()->inProgress()) {
            for (const auto& items : missions)
                qDeleteAll(items);
            return false;
        }
        const double bandMin = -widthMeters / 2.0 + bandOfVehicle[v] * bandWidth;
        const double bandMax = bandMin + bandWidth;
        const QGeoCoordinate homeCoord = targets[v]->coordinate().isValid() ? targets[v]->coordinate() : center;

        QList<MissionItem*> items =
            buildCoverageMissionItems(target, homeCoord, center, lengthMeters, widthMeters, angleDeg, bandMin, bandMax);
        totalWaypoints += items.count() - 3;  // 去掉起飞/返航项

        // 与人工 Plan 视图“上传”相同的通道：PlanManager::writeMissionItems
        // （QGC MissionController 上传人工任务时调用的正是此函数）
        missions.append(items);
    }
    // Register every member before any synchronous rejection can complete the batch.
    for (int v = 0; v < vehicleCount; ++v)
        _trackUpload(targets[v], missions[v], allowed);
    emit missionStatusChanged();
    for (int v = 0; v < vehicleCount; ++v) {
        if (_batchId != batch || _batchFailed || !targets[v] || !_uploads.contains(targets[v]->id()) ||
            targets[v]->missionManager()->inProgress() || (allowed && !allowed(targets[v]->id())) ||
            _batchId != batch || !targets[v] || !_uploads.contains(targets[v]->id())) {
            qDeleteAll(missions[v]);
            if (targets[v] && _batchId == batch)
                _finishUpload(batch, targets[v]->id(), false, tr("上传批次未能完整派发"));
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

void SwarmMissionPlanner::_trackUpload(Vehicle* vehicle, const QList<MissionItem*>& items,
                                       std::function<bool(int)> allowed)
{
    if (!vehicle || !vehicle->missionManager())
        return;
    const int id = vehicle->id();
    auto* context = new QObject(this);
    _uploads.insert(id, context);
    _uploadStates[QString::number(id)] = QStringLiteral("uploading");
    _missionStatus.remove(QString::number(id));
    const auto expected = SwarmMissionSnapshot::capture(items, true);
    QPointer<Vehicle> target(vehicle);
    QPointer<MissionManager> manager(vehicle->missionManager());
    const auto batch = _batchId;
    const auto current = [this, id, batch, target, manager, allowed] {
        // Permission checks can emit signals which cancel this batch or remove the vehicle.
        return batch == _batchId && _uploads.contains(id) && target && manager && (!allowed || allowed(id)) &&
               batch == _batchId && _uploads.contains(id) && target && manager;
    };
    const auto finish = [this, id, batch](bool success, const QString& reason) {
        _finishUpload(batch, id, success, reason);
    };
    const bool sendsHome = vehicle->firmwarePlugin()->sendHomePositionToVehicle();
    connect(vehicle, &QObject::destroyed, context, [finish] { finish(false, tr("载具断开")); });
    connect(manager, &MissionManager::error, context, [finish](int, const QString& reason) { finish(false, reason); });
    connect(
        manager, &MissionManager::sendComplete, context,
        [this, id, manager, context, expected, sendsHome, current, finish](bool error) {
            if (error || !current()) {
                finish(false, tr("上传失败"));
                return;
            }
            _uploadStates[QString::number(id)] = QStringLiteral("reading_back");
            connect(
                manager, &MissionManager::newMissionItemsAvailable, context,
                [this, id, manager, expected, sendsHome, current, finish](bool removed) {
                    if (removed || !current() ||
                        !expected.equivalent(SwarmMissionSnapshot::capture(manager->missionItems(), sendsHome))) {
                        finish(false, tr("机载任务回读与上传内容不一致"));
                        return;
                    }
                    const int count = manager->missionItems().size();
                    _verifiedRevisions[id] = manager->executionRevision();
                    _verifiedSnapshots[id] = SwarmMissionSnapshot::capture(manager->missionItems(), sendsHome);
                    _missionStatus[QString::number(id)] = count;
                    emit missionUploaded(id, count);
                    finish(true, {});
                },
                Qt::SingleShotConnection);
            manager->loadFromVehicle();
            emit missionStatusChanged();
        },
        Qt::SingleShotConnection);
    QTimer::singleShot(120000, context, [finish] { finish(false, tr("上传/回读超时")); });
}

void SwarmMissionPlanner::_finishUpload(quint64 batch, int vehicleId, bool success, const QString& reason,
                                        bool withdrawn)
{
    if (batch != _batchId)
        return;
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
    const auto completedBatch = _batchId;
    const bool complete = _uploads.isEmpty();
    const bool batchSuccess = !_batchFailed && _verifiedCount > 0;
    const QString batchReason = _verifiedCount == 0 && !_batchFailed ? tr("全部任务读写成员已退出") : reason;
    emit missionStatusChanged();
    if (!reason.isEmpty())
        emit plannerFeedback(tr("%1 号机：%2").arg(vehicleId).arg(reason));
    if (complete)
        emit batchFinished(completedBatch, batchSuccess, batchReason);
}

void SwarmMissionPlanner::cancelTargets(const QList<int>& ids)
{
    const auto batch = _batchId;
    for (int id : ids)
        _finishUpload(batch, id, false, {}, true);
}

bool SwarmMissionPlanner::clearAllMissions()
{
    QList<int> ids;
    auto* model = MultiVehicleManager::instance()->vehicles();
    for (int i = 0; i < model->count(); ++i) {
        const auto* vehicle = qobject_cast<Vehicle*>(model->get(i));
        if (vehicle && (!_vehicleAutomationAllowed || _vehicleAutomationAllowed(vehicle->id())))
            ids.append(vehicle->id());
    }
    return readMissionBatch(ids, true, _vehicleAutomationAllowed);
}

bool SwarmMissionPlanner::readMissionBatch(const QList<int>& ids, bool clearFirst, std::function<bool(int)> allowed)
{
    if (busy() || ids.isEmpty())
        return false;
    QHash<int, QPointer<Vehicle>> targets;
    const auto previousBatch = _batchId;
    for (int id : ids) {
        QPointer<Vehicle> vehicle = MultiVehicleManager::instance()->getVehicleById(id);
        if (!vehicle || !vehicle->missionManager() || vehicle->missionManager()->inProgress() ||
            (allowed && !allowed(id)) || !vehicle || busy() || previousBatch != _batchId || targets.contains(id))
            return false;
        targets[id] = vehicle;
    }
    ++_batchId;
    _batchFailed = false;
    _verifiedCount = 0;
    _verifiedRevisions.clear();
    _verifiedSnapshots.clear();
    _uploadStates.clear();
    // Register the entire batch first; even a synchronous first failure must not complete a partial batch.
    for (int id : ids)
        _uploads[id] = new QObject(this);
    const auto batch = _batchId;
    for (int id : ids) {
        if (_batchId != batch || !_uploads.contains(id))
            continue;
        const auto target = targets.value(id);
        QPointer<MissionManager> manager = target ? target->missionManager() : nullptr;
        QObject* context = _uploads.value(id);
        const auto current = [this, id, batch, target, manager, allowed] {
            return batch == _batchId && _uploads.contains(id) && target && manager && (!allowed || allowed(id)) &&
                   batch == _batchId && _uploads.contains(id) && target && manager;
        };
        const auto finish = [this, id, batch](bool success, const QString& reason) {
            _finishUpload(batch, id, success, reason);
        };
        if (!current() || manager->inProgress()) {
            finish(false, tr("任务读写目标或许可已变化"));
            continue;
        }
        auto* timeout = new QTimer(context);
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, context, [finish] { finish(false, tr("任务清空/回读超时")); });
        connect(target, &QObject::destroyed, context, [finish] { finish(false, tr("载具断开")); });
        connect(manager, &MissionManager::error, context,
                [finish](int, const QString& reason) { finish(false, reason); });
        const auto readBack = [this, id, target, manager, context, timeout, clearFirst, current, finish] {
            if (!current() || manager->inProgress()) {
                finish(false, tr("任务回读目标或许可已变化"));
                return;
            }
            _uploadStates[QString::number(id)] = QStringLiteral("reading_back");
            const bool hasHome = target->firmwarePlugin()->sendHomePositionToVehicle();
            connect(
                manager, &MissionManager::newMissionItemsAvailable, context,
                [this, id, manager, hasHome, clearFirst, current, finish](bool removed) {
                    if (removed || !current()) {
                        finish(false, tr("任务回读失败或许可已变化"));
                        return;
                    }
                    const auto snapshot = SwarmMissionSnapshot::capture(manager->missionItems(), hasHome);
                    if (clearFirst && !snapshot.empty()) {
                        finish(false, tr("飞控确认清空后回读仍有任务项"));
                        return;
                    }
                    _verifiedSnapshots[id] = snapshot;
                    _verifiedRevisions[id] = manager->executionRevision();
                    _missionStatus[QString::number(id)] = manager->missionItems().size();
                    finish(true, {});
                },
                Qt::SingleShotConnection);
            timeout->start(120000);
            manager->loadFromVehicle();
        };
        if (clearFirst) {
            _uploadStates[QString::number(id)] = QStringLiteral("clearing");
            connect(
                manager, &MissionManager::removeAllComplete, context,
                [finish, readBack](bool error) {
                    if (error)
                        finish(false, tr("飞控拒绝清空任务"));
                    else
                        readBack();
                },
                Qt::SingleShotConnection);
            timeout->start(15000);
            manager->removeAll();
        } else {
            readBack();
        }
    }
    emit missionStatusChanged();
    return true;
}
