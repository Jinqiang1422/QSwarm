#include "FormationController.h"

#include <QtPositioning/QGeoCoordinate>
#include <algorithm>
#include <cmath>
#include <utility>

#include "Fact.h"
#include "FirmwarePlugin.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "SwarmClock.h"
#include "Vehicle.h"
#include "VehicleFactGroup.h"

FormationController::FormationController(QObject* parent) : QObject(parent)
{
    _updateTimer.setInterval(_updateIntervalMs);
    (void) connect(&_updateTimer, &QTimer::timeout, this, &FormationController::_updateFormation);
    _setStatusText(tr("未激活"));
}

FormationController::~FormationController() = default;

void FormationController::setVehicleAutomationAllowedCallback(std::function<bool(int)> callback)
{
    _vehicleAutomationAllowed = std::move(callback);
}

void FormationController::setTargetAllowedCallback(std::function<bool(int, const QGeoCoordinate&)> callback)
{
    _targetAllowed = std::move(callback);
}

void FormationController::setMembers(const QSet<int>& members)
{
    _members = members;
    _membersBound = true;
    _slots.clear();
    _guidanceTargets.clear();
}

void FormationController::removeMembers(const QSet<int>& members)
{
    _members.subtract(members);
    for (int id : members) {
        _slots.remove(id);
        _guidanceTargets.remove(id);
    }
    if (!_active)
        return;
    if (_members.size() < 2) {
        if (!_members.isEmpty()) {
            _leaderVehicleId = *_members.begin();
            emit leaderVehicleIdChanged();
        }
        stop();
    } else if (members.contains(_leaderVehicleId)) {
        if (!_handover())
            _setStatusText(tr("编队阻塞：没有合格继任长机"));
    }
}

bool FormationController::_handover()
{
    auto ids = _members.values();
    std::sort(ids.begin(), ids.end());
    for (int id : ids) {
        Vehicle* leader = MultiVehicleManager::instance()->getVehicleById(id);
        if (!leader || !leader->armed() || !leader->coordinate().isValid() ||
            (_vehicleAutomationAllowed && !_vehicleAutomationAllowed(id)))
            continue;
        auto* facts = qobject_cast<VehicleFactGroup*>(leader->vehicleFactGroup());
        if (!facts || facts->altitudeRelative()->rawValue().toDouble() < _minAirborneAltMeters)
            continue;
        const double heading = facts->heading()->rawValue().toDouble();
        const double altitude = facts->altitudeAMSL()->rawValue().toDouble();
        _leaderVehicleId = id;
        _slots.clear();
        _guidanceTargets.clear();
        for (int member : ids) {
            Vehicle* follower = MultiVehicleManager::instance()->getVehicleById(member);
            if (!follower || member == id || !follower->coordinate().isValid())
                continue;
            auto* followerFacts = qobject_cast<VehicleFactGroup*>(follower->vehicleFactGroup());
            if (!followerFacts)
                continue;
            _slots.insert(member, {leader->coordinate().distanceTo(follower->coordinate()),
                                   leader->coordinate().azimuthTo(follower->coordinate()) - heading,
                                   followerFacts->altitudeAMSL()->rawValue().toDouble() - altitude});
        }
        emit leaderVehicleIdChanged();
        _setStatusText(tr("长机交接至 %1，保持成员当前位置关系").arg(id));
        return true;
    }
    return false;
}

void FormationController::setFormationType(FormationType type)
{
    if (_formationType != type) {
        _formationType = type;
        if (_active) {
            _rebuildSlots();
        }
        emit formationTypeChanged();
    }
}

void FormationController::setSpacingMeters(double spacing)
{
    if (!qFuzzyCompare(_spacingMeters, spacing)) {
        _spacingMeters = qMax(2.0, spacing);  // 硬性下限 2 米，防止误输入导致碰撞
        if (_active) {
            _rebuildSlots();
        }
        emit spacingMetersChanged();
    }
}

void FormationController::setVerticalStepMeters(double step)
{
    if (!qFuzzyCompare(_verticalStepMeters, step)) {
        _verticalStepMeters = step;
        if (_active) {
            _rebuildSlots();
        }
        emit verticalStepMetersChanged();
    }
}

bool FormationController::start(int leaderVehicleId)
{
    if (_active) {
        stop();
    }

    MultiVehicleManager* mvm = MultiVehicleManager::instance();
    if (_membersBound && !_members.contains(leaderVehicleId))
        return false;
    Vehicle* leader = mvm->getVehicleById(leaderVehicleId);
    if (!leader) {
        _setStatusText(tr("启动失败：找不到长机 %1").arg(leaderVehicleId));
        return false;
    }
    if (!leader->armed() || !leader->coordinate().isValid()) {
        _setStatusText(tr("启动失败：长机 %1 未解锁或定位无效").arg(leaderVehicleId));
        return false;
    }

    _leaderVehicleId = leaderVehicleId;
    _blocked = false;
    _active = true;
    if (!_membersBound) {
        for (int i = 0; i < mvm->vehicles()->count(); ++i) {
            auto* member = qobject_cast<Vehicle*>(mvm->vehicles()->get(i));
            if (member && (!_vehicleAutomationAllowed || _vehicleAutomationAllowed(member->id())))
                _members.insert(member->id());
        }
    }
    _rebuildSlots();
    emit leaderVehicleIdChanged();
    emit activeChanged();
    _updateFormation();  // 立即执行一拍
    _updateTimer.start();
    return true;
}

void FormationController::_rebuildSlots()
{
    auto members = _members.values();
    members.removeAll(_leaderVehicleId);
    std::sort(members.begin(), members.end());
    _slots.clear();
    // Discard targets from the previous shape, even if the aircraft never reached them.
    // The first update initializes each follower at its measured position and altitude.
    _guidanceTargets.clear();
    for (int i = 0; i < members.size(); ++i) {
        Slot slot;
        _formationOffset(i + 1, members.size(), slot.distance, slot.bearing, slot.altitude);
        _slots.insert(members[i], slot);
    }
}

void FormationController::stop()
{
    _guidanceTargets.clear();
    if (!_active) {
        return;
    }
    _updateTimer.stop();
    _blocked = false;
    _active = false;
    _followerCount = 0;
    emit activeChanged();
    emit followerCountChanged();
    _setStatusText(tr("已停止"));
}

void FormationController::_updateFormation()
{
    if (!_active)
        return;
    MultiVehicleManager* mvm = MultiVehicleManager::instance();
    if (!_members.contains(_leaderVehicleId) && !_handover()) {
        _blocked = true;
        _guidanceTargets.clear();
        emit activeChanged();
        _setStatusText(tr("编队阻塞：没有合格继任长机"));
        return;
    }
    Vehicle* leader = mvm->getVehicleById(_leaderVehicleId);

    // 长机不满足引导条件时阻塞自动引导，由各机保持当前飞行模式。
    if (!leader || !leader->armed() || !leader->coordinate().isValid()) {
        _blocked = true;
        _guidanceTargets.clear();
        emit activeChanged();
        _setStatusText(tr("长机 %1 状态异常，编队引导已阻塞").arg(_leaderVehicleId));
        return;
    }
    _blocked = false;

    const QGeoCoordinate leaderCoord = leader->coordinate();
    VehicleFactGroup* leaderFactGroup = qobject_cast<VehicleFactGroup*>(leader->vehicleFactGroup());
    const double leaderHeading = leaderFactGroup ? leaderFactGroup->heading()->rawValue().toDouble() : 0.0;
    const double leaderAltAMSL = leaderFactGroup ? leaderFactGroup->altitudeAMSL()->rawValue().toDouble() : 0.0;
    if (!leaderFactGroup || !std::isfinite(leaderHeading) || !std::isfinite(leaderAltAMSL)) {
        _blocked = true;
        _guidanceTargets.clear();
        emit activeChanged();
        _setStatusText(tr("长机位置或高度无效，编队引导已阻塞"));
        return;
    }

    // 收集僚机：除长机外已解锁且已离地的在线车辆，按 id 升序保证槽位稳定
    QList<Vehicle*> followers;
    QmlObjectListModel* vehicles = mvm->vehicles();
    for (int i = 0; i < vehicles->count(); i++) {
        Vehicle* vehicle = qobject_cast<Vehicle*>(vehicles->get(i));
        if (!vehicle || !_members.contains(vehicle->id()) || vehicle->id() == _leaderVehicleId || !vehicle->armed() ||
            (_vehicleAutomationAllowed && !_vehicleAutomationAllowed(vehicle->id()))) {
            if (vehicle)
                _guidanceTargets.remove(vehicle->id());
            continue;
        }
        VehicleFactGroup* vfg = qobject_cast<VehicleFactGroup*>(vehicle->vehicleFactGroup());
        const double altRel = vfg ? vfg->altitudeRelative()->rawValue().toDouble() : 0.0;
        if (!std::isfinite(altRel) || altRel < _minAirborneAltMeters) {
            _guidanceTargets.remove(vehicle->id());
            continue;  // 未离地，等待下一个周期自动入队
        }
        followers.append(vehicle);
    }
    std::sort(followers.begin(), followers.end(), [](const Vehicle* a, const Vehicle* b) { return a->id() < b->id(); });

    // 槽位自愈：成员在队却无槽位（长机交接瞬间坐标无效/离线、运行中刷新成员）时，
    // 静默跳过会让该机永久脱离编队引导，而僚机计数与状态文本仍报一切正常。
    const bool missingSlots = std::any_of(followers.cbegin(), followers.cend(),
                                          [this](Vehicle* follower) { return !_slots.contains(follower->id()); });
    if (missingSlots) {
        _rebuildSlots();
        _setStatusText(tr("检测到无槽位编队成员，已重建队形槽位"));
    }

    bool transitioning = false;
    const qint64 nowMs = SwarmClock::nowMs();
    for (int k = 0; k < followers.count(); k++) {
        double distanceMeters = 0.0, azimuthOffsetDeg = 0.0, altitudeStepMeters = 0.0;
        if (!_slots.contains(followers[k]->id()))
            continue;
        const Slot slot = _slots.value(followers[k]->id());
        distanceMeters = slot.distance;
        azimuthOffsetDeg = slot.bearing;
        altitudeStepMeters = slot.altitude;

        // 目标点 = 长机位置沿 (长机航向 + 方位偏移) 推移 distanceMeters，高度 = 长机 AMSL + 高度分层
        QGeoCoordinate destination = leaderCoord.atDistanceAndAzimuth(distanceMeters, leaderHeading + azimuthOffsetDeg);
        destination.setAltitude(leaderAltAMSL + altitudeStepMeters);
        Vehicle* follower = followers[k];
        QGeoCoordinate measured = follower->coordinate();
        measured.setAltitude(follower->altitudeAMSL()->rawValue().toDouble());
        if (!measured.isValid() || !std::isfinite(measured.altitude()) || !destination.isValid() ||
            !std::isfinite(destination.altitude())) {
            _guidanceTargets.remove(follower->id());
            _blocked = true;
            _setStatusText(tr("编队引导阻塞：%1 号机位置或高度无效").arg(follower->id()));
            continue;
        }
        // 上一条重定位指令仍在等待 ACK（链路慢，或该机正在执行其他引导指令）时本拍跳过：
        // 否则命令队列以重复指令拒发，每个周期都向操作员弹出错误。
        if (follower->isMavCommandPending(follower->defaultComponentId(), MAV_CMD_DO_REPOSITION))
            continue;
        if (!_guidanceTargets.contains(follower->id()))
            _guidanceTargets.insert(follower->id(), {measured, nowMs, QGeoCoordinate()});
        auto& guidance = _guidanceTargets[follower->id()];
        // Never catch up a delayed timer with a large command, or accumulate motion while blocked.
        const double dt = std::clamp(nowMs - guidance.updatedMs, qint64(0), qint64(_updateIntervalMs)) / 1000.0;
        guidance.updatedMs = nowMs;
        const QGeoCoordinate target = advanceGuidanceTarget(guidance.coordinate, guidance.destination, destination, dt);
        transitioning |=
            target.distanceTo(destination) > 0.1 || std::abs(target.altitude() - destination.altitude()) > 0.1;
        if (!target.isValid() || (_targetAllowed && !_targetAllowed(followers[k]->id(), target))) {
            _blocked = true;
            _setStatusText(tr("编队引导阻塞：%1 号机目标约束未通过").arg(followers[k]->id()));
            continue;
        }

        FirmwarePlugin* firmwarePlugin = followers[k]->firmwarePlugin();
        if (firmwarePlugin) {
            guidance.coordinate = target;
            guidance.destination = destination;
            firmwarePlugin->guidedModeGotoLocationAndAltitude(followers[k], target);
        }
    }

    if (_followerCount != followers.count()) {
        _followerCount = followers.count();
        emit followerCountChanged();
    }
    emit activeChanged();
    if (!_blocked) {
        _setStatusText((transitioning ? tr("编队调整中：从当前位置平滑过渡，长机 %1，僚机 %2 架")
                                      : tr("编队运行中：长机 %1，僚机 %2 架"))
                           .arg(_leaderVehicleId)
                           .arg(_followerCount));
    }
}

QGeoCoordinate FormationController::advanceGuidanceTarget(const QGeoCoordinate& current,
                                                          const QGeoCoordinate& previousDestination,
                                                          const QGeoCoordinate& destination, double dtSeconds)
{
    if (!current.isValid() || !destination.isValid() || !(dtSeconds > 0)) {
        return current;
    }
    QGeoCoordinate base = current;
    if (previousDestination.isValid()) {
        // 槽位目的地随长机移动：跟随这部分运动是编队飞行的本体，不限速。
        base = current.atDistanceAndAzimuth(previousDestination.distanceTo(destination),
                                            previousDestination.azimuthTo(destination));
        base.setAltitude(current.altitude() + (destination.altitude() - previousDestination.altitude()));
    }
    // 朝槽位的追赶保持限速：单拍指令不大幅跳变，时钟跳变不被放大。
    const double catchUp = std::min(base.distanceTo(destination), _horizontalTargetSpeed * dtSeconds);
    QGeoCoordinate target = base.atDistanceAndAzimuth(catchUp, base.azimuthTo(destination));
    target.setAltitude(base.altitude() + std::clamp(destination.altitude() - base.altitude(),
                                                    -_verticalTargetSpeed * dtSeconds,
                                                    _verticalTargetSpeed * dtSeconds));
    return target;
}

void FormationController::_formationOffset(int followerIndex, int totalFollowers, double& distanceMeters,
                                           double& azimuthOffsetDeg, double& altitudeStepMeters) const
{
    const int rank = (followerIndex + 1) / 2;             // 排数（1 起）
    const int side = (followerIndex % 2 == 1) ? -1 : +1;  // 奇数槽位在左，偶数在右

    switch (_formationType) {
        case FormationWedge:  // 楔形：长机后方 ±30° 逐排展开，逐排升高
            distanceMeters = _spacingMeters * rank;
            azimuthOffsetDeg = 180.0 + side * 30.0;
            altitudeStepMeters = _verticalStepMeters * rank;
            break;
        case FormationColumn:  // 纵队：长机正后方依次排开，逐机升高
            distanceMeters = _spacingMeters * followerIndex;
            azimuthOffsetDeg = 180.0;
            altitudeStepMeters = _verticalStepMeters * followerIndex;
            break;
        case FormationLine:  // 横队：长机左右两翼展开，同高度
            distanceMeters = _spacingMeters * rank;
            azimuthOffsetDeg = side * 90.0;
            altitudeStepMeters = 0.0;
            break;
        case FormationCircle:  // 环形：僚机均布在长机周围，同高度
            distanceMeters = _spacingMeters;
            azimuthOffsetDeg = (totalFollowers > 0) ? (360.0 * (followerIndex - 1) / totalFollowers) : 0.0;
            altitudeStepMeters = 0.0;
            break;
    }
}

void FormationController::_setStatusText(const QString& text)
{
    if (_statusText != text) {
        _statusText = text;
        emit statusTextChanged();
    }
}
