#include "FormationController.h"

#include <QtPositioning/QGeoCoordinate>
#include <algorithm>
#include <cmath>
#include <utility>

#include "Fact.h"
#include "FirmwarePlugin.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "SwarmAuditLogger.h"
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

void FormationController::setTransitConstraintsCallback(std::function<TransitConstraints()> callback)
{
    _transitConstraints = std::move(callback);
}

void FormationController::setMembers(const QSet<int>& members)
{
    _finishTransit(false, tr("编队成员已变更，航段已取消"));
    _members = members;
    _membersBound = true;
    _slots.clear();
    _guidanceTargets.clear();
}

void FormationController::removeMembers(const QSet<int>& members)
{
    _members.subtract(members);
    if (members.contains(_leaderVehicleId))
        _finishTransit(false, tr("长机 %1 离队，航段已取消").arg(_leaderVehicleId));
    for (int id : members) {
        _slots.remove(id);
        _guidanceTargets.remove(id);
        // 剩余机对的路径已两两校验，僚机离队后其余成员继续当前航段
        _tracker.removeMember(id);
        _awaitingAck.remove(id);
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
        if (!leader || !leader->armed() || !leader->coordinate().isValid() || _inFailsafe(leader) ||
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
        int index = 0;
        for (int member : ids) {
            Vehicle* follower = MultiVehicleManager::instance()->getVehicleById(member);
            if (!follower || member == id || !follower->coordinate().isValid())
                continue;
            auto* followerFacts = qobject_cast<VehicleFactGroup*>(follower->vehicleFactGroup());
            if (!followerFacts)
                continue;
            _slots.insert(member, {leader->coordinate().distanceTo(follower->coordinate()),
                                   leader->coordinate().azimuthTo(follower->coordinate()) - heading,
                                   followerFacts->altitudeAMSL()->rawValue().toDouble() - altitude, index++});
        }
        _formationHeading = std::isfinite(heading) ? heading : _formationHeading;
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
        _onShapeChanged();
        emit formationTypeChanged();
    }
}

void FormationController::setSpacingMeters(double spacing)
{
    if (!qFuzzyCompare(_spacingMeters, spacing)) {
        _spacingMeters = qMax(2.0, spacing);  // 硬性下限 2 米，防止误输入导致碰撞
        _onShapeChanged();
        emit spacingMetersChanged();
    }
}

void FormationController::setVerticalStepMeters(double step)
{
    if (!qFuzzyCompare(_verticalStepMeters, step)) {
        _verticalStepMeters = step;
        _onShapeChanged();
        emit verticalStepMetersChanged();
    }
}

void FormationController::_onShapeChanged()
{
    if (!_active)
        return;
    _finishTransit(false, tr("队形参数已变更，航段已取消"));
    _rebuildSlots();
    _reshapePending = true;
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
    auto* leaderFacts = qobject_cast<VehicleFactGroup*>(leader->vehicleFactGroup());
    const double heading = leaderFacts ? leaderFacts->heading()->rawValue().toDouble() : 0.0;
    _formationHeading = std::isfinite(heading) ? heading : 0.0;
    if (!_membersBound) {
        for (int i = 0; i < mvm->vehicles()->count(); ++i) {
            auto* member = qobject_cast<Vehicle*>(mvm->vehicles()->get(i));
            if (member && (!_vehicleAutomationAllowed || _vehicleAutomationAllowed(member->id())))
                _members.insert(member->id());
        }
    }
    _rebuildSlots();
    _reshapePending = true;
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
        slot.index = i;
        _slots.insert(members[i], slot);
    }
}

void FormationController::stop()
{
    // 先停定时器并置为未激活，再结束航段：transitFinished 的接收方若同步重入（再次 goto、再次启动等），
    // 看到的已是停止后的状态，不会在停止过程中发起新的引导。
    const bool wasActive = _active;
    if (wasActive) {
        _updateTimer.stop();
        _active = false;
    }
    _finishTransit(false, tr("编队已停止：各机继续飞向当前航段终点，如需原地悬停请用「暂停」"));
    _reshapePending = false;
    _guidanceTargets.clear();
    if (!wasActive) {
        return;
    }
    _blocked = false;
    _followerCount = 0;
    emit activeChanged();
    emit followerCountChanged();
    _setStatusText(tr("已停止"));
}

void FormationController::_block(const QString& text)
{
    _finishTransit(false, text);
    _blocked = true;
    _guidanceTargets.clear();
    emit activeChanged();
    _setStatusText(text);
}

bool FormationController::_inFailsafe(const Vehicle* vehicle)
{
    // PX4 在失效保护期间以 HEARTBEAT system_status = MAV_STATE_CRITICAL 上报；EMERGENCY、POWEROFF、
    // FLIGHT_TERMINATION 更严重。每条带 CHANGE_MODE 的 DO_REPOSITION 都是一次模式切换请求，
    // 在悬停延迟窗口 COM_FAIL_ACT_T 内收到会被 PX4 当作用户接管，取消正在进行的返航/降落。
    // HEARTBEAT 为 1 Hz：失效保护开始后最长约 1 秒（另加链路延迟）这里仍返回 false，窗口缩小而未消除。
    return vehicle && vehicle->systemStatus() >= MAV_STATE_CRITICAL;
}

QString FormationController::transitBlockedReason() const
{
    return _transitBlockedReason(MultiVehicleManager::instance()->getVehicleById(_leaderVehicleId));
}

QString FormationController::_transitBlockedReason(Vehicle* leader) const
{
    // 只看长机级条件：单架僚机的保持目标无效会置 _blocked，但不妨碍全队航段（该僚机由规划与路径检查处理）
    if (!_active)
        return tr("编队未运行");
    if (!_members.contains(_leaderVehicleId))
        return tr("长机 %1 不在编队成员中").arg(_leaderVehicleId);
    if (!leader || !leader->armed() || !leader->coordinate().isValid())
        return tr("长机 %1 状态异常").arg(_leaderVehicleId);
    if (_inFailsafe(leader))
        return tr("长机 %1 处于失效保护").arg(_leaderVehicleId);
    if (_vehicleAutomationAllowed && !_vehicleAutomationAllowed(_leaderVehicleId))
        return tr("长机 %1 未获自动控制许可").arg(_leaderVehicleId);
    return {};
}

bool FormationController::guides(int vehicleId) const
{
    if (!_active)
        return false;
    // 长机离队且无合格继任时，_leaderVehicleId 仍指向已离队的原长机：它已不受编队引导，goto 应走原生路径
    if (vehicleId == _leaderVehicleId)
        return _members.contains(vehicleId);
    const QList<Vehicle*> followers = _eligibleFollowers();
    return std::any_of(followers.cbegin(), followers.cend(),
                       [vehicleId](const Vehicle* follower) { return follower->id() == vehicleId; });
}

QList<Vehicle*> FormationController::_eligibleFollowers() const
{
    QList<Vehicle*> followers;
    QmlObjectListModel* vehicles = MultiVehicleManager::instance()->vehicles();
    for (int i = 0; i < vehicles->count(); i++) {
        Vehicle* vehicle = qobject_cast<Vehicle*>(vehicles->get(i));
        if (!vehicle || !_members.contains(vehicle->id()) || vehicle->id() == _leaderVehicleId || !vehicle->armed() ||
            _inFailsafe(vehicle) || (_vehicleAutomationAllowed && !_vehicleAutomationAllowed(vehicle->id())))
            continue;
        VehicleFactGroup* vfg = qobject_cast<VehicleFactGroup*>(vehicle->vehicleFactGroup());
        const double altRel = vfg ? vfg->altitudeRelative()->rawValue().toDouble() : 0.0;
        if (!std::isfinite(altRel) || altRel < _minAirborneAltMeters)
            continue;  // 未离地，等待下一个周期自动入队
        followers.append(vehicle);
    }
    std::sort(followers.begin(), followers.end(), [](const Vehicle* a, const Vehicle* b) { return a->id() < b->id(); });
    return followers;
}

bool FormationController::_leaderMoving(Vehicle* leader) const
{
    auto* facts = leader ? qobject_cast<VehicleFactGroup*>(leader->vehicleFactGroup()) : nullptr;
    const double speed = facts ? facts->groundSpeed()->rawValue().toDouble() : 0.0;
    return std::isfinite(speed) && speed > _leaderMovingSpeed;
}

double FormationController::_maxHorizontalSpeed(const QList<Vehicle*>& vehicles) const
{
    double limit = NAN;
    for (Vehicle* vehicle : vehicles) {
        FirmwarePlugin* plugin = vehicle ? vehicle->firmwarePlugin() : nullptr;
        const double speed = plugin ? plugin->maximumHorizontalSpeedMultirotorMetersSecond(vehicle) : NAN;
        if (std::isfinite(speed) && speed > 0)
            limit = std::isfinite(limit) ? std::min(limit, speed) : speed;
    }
    return limit;
}

void FormationController::_updateFormation()
{
    if (!_active)
        return;
    MultiVehicleManager* mvm = MultiVehicleManager::instance();
    if (!_members.contains(_leaderVehicleId) && !_handover()) {
        _block(tr("编队阻塞：没有合格继任长机"));
        return;
    }
    Vehicle* leader = mvm->getVehicleById(_leaderVehicleId);

    // 长机不满足引导条件时阻塞自动引导，由各机保持当前飞行模式。
    if (!leader || !leader->armed() || !leader->coordinate().isValid()) {
        _block(tr("长机 %1 状态异常，编队引导已阻塞").arg(_leaderVehicleId));
        return;
    }
    if (_inFailsafe(leader)) {
        _block(tr("长机 %1 处于失效保护，编队引导已阻塞").arg(_leaderVehicleId));
        return;
    }
    _blocked = false;

    const QGeoCoordinate leaderCoord = leader->coordinate();
    VehicleFactGroup* leaderFactGroup = qobject_cast<VehicleFactGroup*>(leader->vehicleFactGroup());
    const double leaderHeading = leaderFactGroup ? leaderFactGroup->heading()->rawValue().toDouble() : 0.0;
    const double leaderAltAMSL = leaderFactGroup ? leaderFactGroup->altitudeAMSL()->rawValue().toDouble() : 0.0;
    if (!leaderFactGroup || !std::isfinite(leaderHeading) || !std::isfinite(leaderAltAMSL)) {
        _block(tr("长机位置或高度无效，编队引导已阻塞"));
        return;
    }

    // 僚机：除长机外已解锁且已离地的成员，按 id 升序保证槽位稳定
    const QList<Vehicle*> followers = _eligibleFollowers();
    const QList<int> guidedIds = _guidanceTargets.keys();
    for (int id : guidedIds) {
        if (std::none_of(followers.cbegin(), followers.cend(),
                         [id](const Vehicle* follower) { return follower->id() == id; }))
            _guidanceTargets.remove(id);
    }
    if (_followerCount != followers.count()) {
        _followerCount = followers.count();
        emit followerCountChanged();
    }

    // 航段执行中由航段跟踪器指挥全队；航段期间新离地的成员待航段结束后再入队。
    if (transitActive()) {
        _updateTransit();
        emit activeChanged();
        return;
    }

    // 槽位自愈：成员在队却无槽位（长机交接瞬间坐标无效/离线、运行中刷新成员）时，
    // 静默跳过会让该机永久脱离编队引导，而僚机计数与状态文本仍报一切正常。
    const bool missingSlots = std::any_of(followers.cbegin(), followers.cend(),
                                          [this](Vehicle* follower) { return !_slots.contains(follower->id()); });
    if (missingSlots) {
        _rebuildSlots();
        // 重建后的槽位与各机当前位置无关，能同步收敛时走原地航段，否则由下面的逐机追赶接管
        _reshapePending = true;
        _setStatusText(tr("检测到无槽位编队成员，已重建队形槽位"));
    }

    // 启动或队形参数变更后，全队同步收敛到新槽位；不可行时退回下面的逐机追赶。
    // 仅因长机在动而无法发起时保留待办，长机停下后再试；规划失败、没有僚机则不再重试。
    if (_reshapePending) {
        _reshapePending = !followers.isEmpty() && _leaderMoving(leader);
        if (!_reshapePending && _beginReshape(leader, followers)) {
            emit activeChanged();
            return;
        }
    }

    // 编队朝向：长机由其他方式驱动时跟随长机航向，否则保持上一航段结束时的朝向，
    // 避免长机悬停时的偏航带动整个队形甩动。
    if (_leaderMoving(leader))
        _formationHeading = leaderHeading;

    bool transitioning = false;
    const qint64 nowMs = SwarmClock::nowMs();
    for (Vehicle* follower : followers) {
        if (!_slots.contains(follower->id()))
            continue;
        const Slot slot = _slots.value(follower->id());

        // 目标点 = 长机位置沿 (编队朝向 + 方位偏移) 推移 slot.distance，高度 = 长机 AMSL + 高度分层
        QGeoCoordinate destination = leaderCoord.atDistanceAndAzimuth(slot.distance, _formationHeading + slot.bearing);
        destination.setAltitude(leaderAltAMSL + slot.altitude);
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
        if (!target.isValid() || (_targetAllowed && !_targetAllowed(follower->id(), target))) {
            _blocked = true;
            _setStatusText(tr("编队引导阻塞：%1 号机目标约束未通过").arg(follower->id()));
            continue;
        }

        FirmwarePlugin* firmwarePlugin = follower->firmwarePlugin();
        if (firmwarePlugin) {
            guidance.coordinate = target;
            guidance.destination = destination;
            firmwarePlugin->guidedModeGotoLocationAndAltitude(follower, target);
        }
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

bool FormationController::gotoFormation(const QGeoCoordinate& coordinate, QString* reason)
{
    Vehicle* leader = MultiVehicleManager::instance()->getVehicleById(_leaderVehicleId);
    if (!coordinate.isValid() || !leader) {
        if (reason)
            *reason = tr("目标点或长机无效");
        return false;
    }
    const QGeoCoordinate target(coordinate.latitude(), coordinate.longitude(),
                                leader->altitudeAMSL()->rawValue().toDouble());
    return _planTransit({target}, QStringLiteral("goto"), reason);
}

bool FormationController::startRoute(const QList<QGeoCoordinate>& waypoints, QString* reason)
{
    if (waypoints.isEmpty()) {
        if (reason)
            *reason = tr("航线没有可执行的航点");
        return false;
    }
    return _planTransit(waypoints, QStringLiteral("route"), reason);
}

FormationLegPlanner::Plan FormationController::planRoute(const QList<QGeoCoordinate>& waypoints) const
{
    Vehicle* leader = MultiVehicleManager::instance()->getVehicleById(_leaderVehicleId);
    FormationLegPlanner::Plan plan;
    if (!_active || !leader || waypoints.isEmpty()) {
        plan.error = tr("编队未运行或航线为空");
        return plan;
    }
    // 与 startRoute 的前置条件一致，确认框的预览不会显示一条随后被拒绝的航线
    plan.error = _transitBlockedReason(leader);
    if (!plan.error.isEmpty())
        return plan;
    return FormationLegPlanner::plan(_legRequest(leader, _eligibleFollowers(), waypoints, true));
}

FormationLegPlanner::Request FormationController::_legRequest(Vehicle* leader, const QList<Vehicle*>& followers,
                                                              const QList<QGeoCoordinate>& waypoints,
                                                              bool commandLeader) const
{
    FormationLegPlanner::Request request;
    request.leaderId = _leaderVehicleId;
    // 没有槽位的僚机不参与（如确认框预览时槽位尚未重建），若在空中则按下面的静止障碍处理
    QList<Vehicle*> participants{leader};
    for (Vehicle* follower : followers) {
        if (_slots.contains(follower->id()))
            participants.append(follower);
    }
    QSet<int> participantIds;
    for (Vehicle* vehicle : std::as_const(participants)) {
        QGeoCoordinate position = vehicle->coordinate();
        position.setAltitude(vehicle->altitudeAMSL()->rawValue().toDouble());
        request.members.append({vehicle->id(), position});
        participantIds.insert(vehicle->id());
    }
    // 不参与本次规划但已在空中的编队成员（失效保护返航中、无许可、待入队、无槽位等）：
    // 航段路径须与它们的当前位置保持间距
    for (int id : _members) {
        Vehicle* vehicle = participantIds.contains(id) ? nullptr : MultiVehicleManager::instance()->getVehicleById(id);
        if (!vehicle)
            continue;
        QGeoCoordinate position = vehicle->coordinate();
        position.setAltitude(vehicle->altitudeAMSL()->rawValue().toDouble());
        const double altitudeRelative = vehicle->altitudeRelative()->rawValue().toDouble();
        if (position.isValid() && std::isfinite(position.altitude()) && std::isfinite(altitudeRelative) &&
            altitudeRelative >= _minAirborneAltMeters)
            request.obstacles.append({id, position});
    }
    // 槽位按编号压缩成连续下标；全部持有者写入 currentAssignment，未参与者的槽位标记为保留
    QList<int> holders = _slots.keys();
    std::sort(holders.begin(), holders.end(),
              [this](int a, int b) { return _slots.value(a).index < _slots.value(b).index; });
    for (int holder : std::as_const(holders)) {
        const Slot slot = _slots.value(holder);
        const int index = request.slotGeometries.size();
        request.slotGeometries.append({slot.distance, slot.bearing, slot.altitude});
        request.currentAssignment.insert(holder, index);
        if (!participantIds.contains(holder))
            request.reservedSlots.insert(index);
    }
    request.startReference = request.members.first().position;
    request.startHeadingDeg = _formationHeading;
    request.waypoints = waypoints;
    request.spacing = _spacingMeters;
    const double maxSpeed = _maxHorizontalSpeed(participants);
    request.speed = std::isfinite(maxSpeed) ? std::min(_transitSpeed, _transitSpeedFactor * maxSpeed) : _transitSpeed;
    // 每次规划只取一次约束：最小间距与全部路径检查基于同一份配置快照
    const TransitConstraints constraints = _transitConstraints ? _transitConstraints() : TransitConstraints{};
    request.minSeparation = constraints.minSeparation;
    request.commandLeader = commandLeader;
    request.pathCheck = constraints.pathCheck;
    return request;
}

bool FormationController::_planTransit(const QList<QGeoCoordinate>& waypoints, const QString& kind, QString* reason)
{
    const auto fail = [reason](const QString& text) {
        if (reason)
            *reason = text;
        return false;
    };
    Vehicle* leader = MultiVehicleManager::instance()->getVehicleById(_leaderVehicleId);
    const QString blocked = _transitBlockedReason(leader);
    if (!blocked.isEmpty())
        return fail(blocked);
    const QList<Vehicle*> followers = _eligibleFollowers();
    if (std::any_of(followers.cbegin(), followers.cend(),
                    [this](Vehicle* follower) { return !_slots.contains(follower->id()); }))
        _rebuildSlots();
    const FormationLegPlanner::Request request = _legRequest(leader, followers, waypoints, true);
    const FormationLegPlanner::Plan plan = FormationLegPlanner::plan(request);
    if (!plan.ok())
        return fail(plan.error);
    _finishTransit(false, tr("新的编队航段已替换当前航段"), false);
    _beginTransit(plan, request, kind);
    return true;
}

bool FormationController::_beginReshape(Vehicle* leader, const QList<Vehicle*>& followers)
{
    if (followers.isEmpty() || _leaderMoving(leader))
        return false;
    QGeoCoordinate reference = leader->coordinate();
    reference.setAltitude(leader->altitudeAMSL()->rawValue().toDouble());
    const FormationLegPlanner::Request request = _legRequest(leader, followers, {reference}, false);
    const FormationLegPlanner::Plan plan = FormationLegPlanner::plan(request);
    if (!plan.ok()) {
        SwarmAuditLogger::instance()->log(QStringLiteral("formation"), QStringLiteral("reshape_fallback"),
                                          {{QStringLiteral("reason"), plan.error}});
        return false;
    }
    _beginTransit(plan, request, QStringLiteral("reshape"));
    return true;
}

void FormationController::_applyAssignment(const FormationLegPlanner::Plan& plan,
                                           const FormationLegPlanner::Request& request)
{
    // 参与者取规划分配，未参与者保留原槽位；编号统一为请求里的压缩下标
    QHash<int, Slot> assigned;
    for (auto it = request.currentAssignment.cbegin(); it != request.currentAssignment.cend(); ++it) {
        const int index = plan.assignment.value(it.key(), it.value());
        const FormationLegPlanner::SlotGeometry& geometry = request.slotGeometries.at(index);
        assigned.insert(it.key(), {geometry.distance, geometry.bearing, geometry.altitude, index});
    }
    for (auto it = plan.assignment.cbegin(); it != plan.assignment.cend(); ++it) {
        if (assigned.contains(it.key()))
            continue;
        const FormationLegPlanner::SlotGeometry& geometry = request.slotGeometries.at(it.value());
        assigned.insert(it.key(), {geometry.distance, geometry.bearing, geometry.altitude, it.value()});
    }
    _slots = assigned;
}

void FormationController::_beginTransit(const FormationLegPlanner::Plan& plan,
                                        const FormationLegPlanner::Request& request, const QString& kind)
{
    _applyAssignment(plan, request);
    _legQueue = plan.legs;
    _legCount = plan.legs.size();
    _legIndex = 0;
    _transitDurationSec = plan.durationSec();
    _transitKind = kind;
    _reshapePending = false;
    _guidanceTargets.clear();
    _transitDropped.clear();
    _transitAccepted.clear();
    _gotoModeSeen.clear();
    QVariantMap slotNumbers;
    for (auto it = plan.assignment.cbegin(); it != plan.assignment.cend(); ++it)
        slotNumbers.insert(QString::number(it.key()), it.value() + 1);
    SwarmAuditLogger::instance()->log(QStringLiteral("formation"), QStringLiteral("leg_planned"),
                                      {{QStringLiteral("kind"), kind},
                                       {QStringLiteral("leg_count"), _legCount},
                                       {QStringLiteral("duration_s"), _transitDurationSec},
                                       {QStringLiteral("speed_mps"), request.speed},
                                       {QStringLiteral("slots"), slotNumbers}});
    if (kind == QStringLiteral("route"))
        SwarmAuditLogger::instance()->log(QStringLiteral("formation"), QStringLiteral("route_started"),
                                          {{QStringLiteral("waypoint_count"), request.waypoints.size()}});
    emit transitChanged();
    _startNextLeg();
}

void FormationController::_startNextLeg()
{
    if (_legQueue.isEmpty())
        return;
    FormationLegPlanner::Leg leg = _legQueue.first();
    // 本次航段/航线中途被移出的成员不再回到后续航段：它们已不在两两校验过的路径集合里
    leg.members.removeIf([this](const FormationLegPlanner::LegMember& member) {
        return !_members.contains(member.id) || _transitDropped.contains(member.id);
    });
    if (leg.members.isEmpty()) {
        _finishTransit(false, tr("航段没有可执行的成员"));
        return;
    }
    ++_legIndex;
    _tracker.start(leg, SwarmClock::nowMs());
    for (const QMetaObject::Connection& connection : std::as_const(_resultConnections))
        disconnect(connection);
    _resultConnections.clear();
    _awaitingAck.clear();
    for (const FormationLegPlanner::LegMember& member : std::as_const(leg.members)) {
        Vehicle* vehicle = MultiVehicleManager::instance()->getVehicleById(member.id);
        if (!vehicle)
            continue;
        _resultConnections.append(connect(
            vehicle, &Vehicle::mavCommandResult, this,
            [this, id = member.id](int, int, int command, int ackResult, int failureCode) {
                // 本地命令队列以重复指令拒发的不是飞控的拒绝，也不是本航段指令的回执
                // （待 ACK 时不再下发），本航段的回执仍在路上。
                if (failureCode == Vehicle::MavCmdResultFailureDuplicateCommand)
                    return;
                // 只认本航段发出的指令的回执：上一航段或保持阶段的迟到回执不得中止本航段。
                // 待 ACK 期间不会再发新指令，所以集合里的 id 对应的回执一定属于本航段。
                if (command != MAV_CMD_DO_REPOSITION || ackResult == MAV_RESULT_IN_PROGRESS || !_awaitingAck.remove(id))
                    return;
                if (ackResult == MAV_RESULT_ACCEPTED) {
                    _transitAccepted.insert(id);
                    return;
                }
                _finishTransit(false, failureCode == Vehicle::MavCmdResultFailureNoResponseToCommand
                                          ? tr("%1 号机未响应航段指令").arg(id)
                                          : tr("%1 号机拒绝航段指令").arg(id));
            }));
    }
    SwarmAuditLogger::instance()->log(QStringLiteral("formation"), QStringLiteral("leg_started"),
                                      {{QStringLiteral("kind"), _transitKind},
                                       {QStringLiteral("index"), _legIndex},
                                       {QStringLiteral("leg_count"), _legCount},
                                       {QStringLiteral("duration_s"), leg.durationSec}});
    emit transitChanged();
    _updateTransit();
}

void FormationController::_updateTransit()
{
    MultiVehicleManager* mvm = MultiVehicleManager::instance();
    // 原地变队形不指挥长机，终点围绕规划时的长机位置；长机一旦被其他方式驱动，终点就不再对应
    // 长机周围的槽位，取消航段，下一拍由保持阶段接管。
    if (_transitKind == QStringLiteral("reshape")) {
        Vehicle* leader = mvm->getVehicleById(_leaderVehicleId);
        if (!leader || _leaderMoving(leader) ||
            leader->coordinate().distanceTo(_tracker.leg().reference) > _reshapeLeaderDriftMeters) {
            _finishTransit(false, tr("长机开始移动，原地变队形已取消"));
            return;
        }
    }
    QHash<int, QGeoCoordinate> positions;
    QSet<int> pending;
    QList<Vehicle*> vehicles;
    const QList<FormationLegPlanner::LegMember> members = _tracker.leg().members;
    for (const FormationLegPlanner::LegMember& member : members) {
        Vehicle* vehicle = mvm->getVehicleById(member.id);
        const bool usable = vehicle && vehicle->armed() && !_inFailsafe(vehicle) &&
                            (!_vehicleAutomationAllowed || _vehicleAutomationAllowed(member.id));
        if (!usable) {
            if (member.id == _leaderVehicleId) {
                _finishTransit(false, tr("长机 %1 失去引导条件，航段已取消").arg(member.id));
                return;
            }
            _tracker.removeMember(member.id);
            _transitDropped.insert(member.id);
            _awaitingAck.remove(member.id);
            continue;
        }
        // 航段指令被接受后，飞控应处于 goto 模式；心跳有延迟，至少观测到一次后才开始判定。此后模式一变
        // （单机降落、返航、开始任务、起飞、切到手动类模式等），说明操作员或任务已接管该机：再发调速
        // 指令会把它拉回 goto 模式、覆盖这次接管，所以僚机移出航段，长机则终止航段。PX4 上单机暂停与
        // 改变高度都走 DO_REPOSITION，仍停留在 Hold（即 goto 模式），不会被判定为模式切换——航段期间
        // 这两种操作仍会被下一次调速指令覆盖，是已知限制。
        FirmwarePlugin* plugin = vehicle->firmwarePlugin();
        const QString gotoMode = plugin ? plugin->gotoFlightMode() : QString();
        const QString mode = vehicle->flightMode();
        if (!gotoMode.isEmpty() && !mode.isEmpty() && _transitAccepted.contains(member.id)) {
            if (mode == gotoMode) {
                _gotoModeSeen.insert(member.id);
            } else if (_gotoModeSeen.contains(member.id)) {
                if (member.id == _leaderVehicleId) {
                    _finishTransit(false, tr("长机 %1 已切换到「%2」，航段已取消").arg(member.id).arg(mode));
                    return;
                }
                _tracker.removeMember(member.id);
                _transitDropped.insert(member.id);
                _awaitingAck.remove(member.id);
                continue;
            }
        }
        QGeoCoordinate position = vehicle->coordinate();
        position.setAltitude(vehicle->altitudeAMSL()->rawValue().toDouble());
        positions.insert(member.id, position);
        if (vehicle->isMavCommandPending(vehicle->defaultComponentId(), MAV_CMD_DO_REPOSITION))
            pending.insert(member.id);
        vehicles.append(vehicle);
    }
    const qint64 nowMs = SwarmClock::nowMs();
    const QList<FormationLegTracker::Command> commands =
        _tracker.update(positions, pending, nowMs, _maxHorizontalSpeed(vehicles));
    switch (_tracker.state()) {
        case FormationLegTracker::State::Completed: {
            if (_tracker.leg().members.isEmpty()) {
                _finishTransit(false, tr("航段没有可执行的成员"));
                return;
            }
            SwarmAuditLogger::instance()->log(QStringLiteral("formation"), QStringLiteral("leg_completed"),
                                              {{QStringLiteral("kind"), _transitKind},
                                               {QStringLiteral("index"), _legIndex},
                                               {QStringLiteral("elapsed_s"), _tracker.elapsedSec(nowMs)},
                                               {QStringLiteral("max_deviation_m"), _tracker.maxDeviationMeters()}});
            _formationHeading = _tracker.leg().headingDeg;
            _legQueue.removeFirst();
            if (_legQueue.isEmpty())
                _finishTransit(true, QString());
            else
                _startNextLeg();
            return;
        }
        case FormationLegTracker::State::TimedOut:
        case FormationLegTracker::State::Aborted:
            _finishTransit(false, _tracker.reason());
            return;
        default:
            break;
    }
    for (const FormationLegTracker::Command& command : commands) {
        Vehicle* vehicle = mvm->getVehicleById(command.id);
        FirmwarePlugin* plugin = vehicle ? vehicle->firmwarePlugin() : nullptr;
        // 发送在本地即失败（链路断开等）时指令不会进入待 ACK 队列：不记为已发，下一拍重试
        if (plugin && plugin->guidedModeGotoLocationAtSpeed(vehicle, command.target, command.speed) &&
            vehicle->isMavCommandPending(vehicle->defaultComponentId(), MAV_CMD_DO_REPOSITION)) {
            _tracker.markSent(command.id, command.speed);
            _awaitingAck.insert(command.id);
        }
    }
    const double remaining = std::max(0.0, _tracker.leg().durationSec - _tracker.elapsedSec(nowMs));
    _setStatusText(tr("编队航段 %1/%2：同步飞行中，预计 %3 秒").arg(_legIndex).arg(_legCount).arg(qRound(remaining)));
}

void FormationController::_finishTransit(bool success, const QString& reason, bool notify)
{
    if (!transitActive())
        return;
    for (const QMetaObject::Connection& connection : std::as_const(_resultConnections))
        disconnect(connection);
    _resultConnections.clear();
    _awaitingAck.clear();
    _transitDropped.clear();
    _transitAccepted.clear();
    _gotoModeSeen.clear();
    const QString kind = _transitKind;
    if (success) {
        if (kind == QStringLiteral("route"))
            SwarmAuditLogger::instance()->log(QStringLiteral("formation"), QStringLiteral("route_completed"),
                                              {{QStringLiteral("leg_count"), _legCount}});
    } else {
        SwarmAuditLogger::instance()->log(
            QStringLiteral("formation"), QStringLiteral("leg_aborted"),
            {{QStringLiteral("kind"), kind}, {QStringLiteral("index"), _legIndex}, {QStringLiteral("reason"), reason}});
        if (kind == QStringLiteral("route"))
            SwarmAuditLogger::instance()->log(QStringLiteral("formation"), QStringLiteral("route_aborted"),
                                              {{QStringLiteral("reason"), reason}});
    }
    _lastTransitResult = {{QStringLiteral("kind"), kind},
                          {QStringLiteral("success"), success},
                          {QStringLiteral("reason"), reason},
                          {QStringLiteral("leg_count"), _legCount},
                          {QStringLiteral("serial"), ++_transitResultSerial}};
    _legQueue.clear();
    _tracker.reset();
    _legIndex = 0;
    _legCount = 0;
    _transitDurationSec = 0;
    _transitKind.clear();
    _guidanceTargets.clear();
    _setStatusText(success ? tr("编队已到位：长机 %1，僚机 %2 架").arg(_leaderVehicleId).arg(_followerCount) : reason);
    emit transitChanged();
    if (notify)
        emit transitFinished(success, reason, kind);
}

QVariantMap FormationController::transitStatus() const
{
    QVariantList members;
    const QHash<int, double> progress = _tracker.progress();
    const QHash<int, double> remaining = _tracker.distanceToEnd();
    for (const FormationLegPlanner::LegMember& member : _tracker.leg().members) {
        members.append(QVariantMap{{QStringLiteral("id"), member.id},
                                   {QStringLiteral("speed_mps"), member.speed},
                                   {QStringLiteral("start_lat"), member.start.latitude()},
                                   {QStringLiteral("start_lon"), member.start.longitude()},
                                   {QStringLiteral("start_alt_m"), member.start.altitude()},
                                   {QStringLiteral("end_lat"), member.end.latitude()},
                                   {QStringLiteral("end_lon"), member.end.longitude()},
                                   {QStringLiteral("end_alt_m"), member.end.altitude()},
                                   {QStringLiteral("progress"), progress.value(member.id)},
                                   {QStringLiteral("distance_to_end_m"), remaining.value(member.id, -1.0)}});
    }
    return {{QStringLiteral("active"), transitActive()},
            {QStringLiteral("kind"), _transitKind},
            {QStringLiteral("leg_index"), _legIndex},
            {QStringLiteral("leg_count"), _legCount},
            {QStringLiteral("elapsed_s"), _tracker.elapsedSec(SwarmClock::nowMs())},
            {QStringLiteral("duration_s"), _tracker.leg().durationSec},
            {QStringLiteral("total_duration_s"), _transitDurationSec},
            {QStringLiteral("consensus_progress"), _tracker.consensusProgress()},
            {QStringLiteral("deviation_m"), _tracker.deviationMeters()},
            {QStringLiteral("max_deviation_m"), _tracker.maxDeviationMeters()},
            {QStringLiteral("members"), members},
            {QStringLiteral("last_result"), _lastTransitResult}};
}

QHash<int, int> FormationController::slotAssignment() const
{
    QHash<int, int> assignment;
    for (auto it = _slots.cbegin(); it != _slots.cend(); ++it)
        assignment.insert(it.key(), it.value().index + 1);
    return assignment;
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
