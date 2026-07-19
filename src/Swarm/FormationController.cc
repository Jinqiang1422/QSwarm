#include "FormationController.h"

#include "MultiVehicleManager.h"
#include "Vehicle.h"
#include "VehicleFactGroup.h"
#include "Fact.h"
#include "QmlObjectListModel.h"

#include <QtPositioning/QGeoCoordinate>

#include <algorithm>

FormationController::FormationController(QObject *parent)
    : QObject(parent)
{
    _updateTimer.setInterval(_updateIntervalMs);
    (void) connect(&_updateTimer, &QTimer::timeout, this, &FormationController::_updateFormation);
    _setStatusText(tr("未激活"));
}

FormationController::~FormationController() = default;

void FormationController::setFormationType(FormationType type)
{
    if (_formationType != type) {
        _formationType = type;
        emit formationTypeChanged();
    }
}

void FormationController::setSpacingMeters(double spacing)
{
    if (!qFuzzyCompare(_spacingMeters, spacing)) {
        _spacingMeters = qMax(2.0, spacing);   // 硬性下限 2 米，防止误输入导致碰撞
        emit spacingMetersChanged();
    }
}

void FormationController::setVerticalStepMeters(double step)
{
    if (!qFuzzyCompare(_verticalStepMeters, step)) {
        _verticalStepMeters = step;
        emit verticalStepMetersChanged();
    }
}

bool FormationController::start(int leaderVehicleId)
{
    if (_active) {
        stop();
    }

    MultiVehicleManager *mvm = MultiVehicleManager::instance();
    Vehicle *leader = mvm->getVehicleById(leaderVehicleId);
    if (!leader) {
        _setStatusText(tr("启动失败：找不到长机 %1").arg(leaderVehicleId));
        return false;
    }
    if (!leader->armed() || !leader->coordinate().isValid()) {
        _setStatusText(tr("启动失败：长机 %1 未解锁或定位无效").arg(leaderVehicleId));
        return false;
    }

    _leaderVehicleId = leaderVehicleId;
    _active = true;
    emit leaderVehicleIdChanged();
    emit activeChanged();
    _updateFormation();      // 立即执行一拍
    _updateTimer.start();
    return true;
}

void FormationController::stop()
{
    if (!_active) {
        return;
    }
    _updateTimer.stop();
    _active = false;
    _followerCount = 0;
    emit activeChanged();
    emit followerCountChanged();
    _setStatusText(tr("已停止"));
}

void FormationController::_updateFormation()
{
    MultiVehicleManager *mvm = MultiVehicleManager::instance();
    Vehicle *leader = mvm->getVehicleById(_leaderVehicleId);

    // 安全：长机丢失 / 上锁 / 定位无效 → 停止编队，僚机原地悬停
    if (!leader || !leader->armed() || !leader->coordinate().isValid()) {
        QmlObjectListModel *vehicles = mvm->vehicles();
        for (int i = 0; i < vehicles->count(); i++) {
            Vehicle *vehicle = qobject_cast<Vehicle *>(vehicles->get(i));
            if (vehicle && vehicle->id() != _leaderVehicleId && vehicle->armed()) {
                vehicle->pauseVehicle();
            }
        }
        _setStatusText(tr("长机 %1 状态异常，编队已自动停止").arg(_leaderVehicleId));
        stop();
        return;
    }

    const QGeoCoordinate leaderCoord  = leader->coordinate();
    const double leaderHeading = leader->vehicleFactGroup()->heading()->rawValue().toDouble();
    const double leaderAltAMSL = leader->vehicleFactGroup()->altitudeAMSL()->rawValue().toDouble();

    // 收集僚机：除长机外已解锁且已离地的在线车辆，按 id 升序保证槽位稳定
    QList<Vehicle *> followers;
    QmlObjectListModel *vehicles = mvm->vehicles();
    for (int i = 0; i < vehicles->count(); i++) {
        Vehicle *vehicle = qobject_cast<Vehicle *>(vehicles->get(i));
        if (!vehicle || vehicle->id() == _leaderVehicleId || !vehicle->armed()) {
            continue;
        }
        const double altRel = vehicle->vehicleFactGroup()->altitudeRelative()->rawValue().toDouble();
        if (altRel < _minAirborneAltMeters) {
            continue;   // 未离地，等待下一个周期自动入队
        }
        followers.append(vehicle);
    }
    std::sort(followers.begin(), followers.end(), [](const Vehicle *a, const Vehicle *b) { return a->id() < b->id(); });

    for (int k = 0; k < followers.count(); k++) {
        double distanceMeters = 0.0, azimuthOffsetDeg = 0.0, altitudeStepMeters = 0.0;
        _formationOffset(k + 1, followers.count(), distanceMeters, azimuthOffsetDeg, altitudeStepMeters);

        // 目标点 = 长机位置沿 (长机航向 + 方位偏移) 推移 distanceMeters，高度 = 长机 AMSL + 高度分层
        QGeoCoordinate target = leaderCoord.atDistanceAndAzimuth(distanceMeters, leaderHeading + azimuthOffsetDeg);
        target.setAltitude(leaderAltAMSL + altitudeStepMeters);

        followers[k]->guidedModeGotoLocation(target);
    }

    if (_followerCount != followers.count()) {
        _followerCount = followers.count();
        emit followerCountChanged();
    }
    _setStatusText(tr("编队运行中：长机 %1，僚机 %2 架").arg(_leaderVehicleId).arg(_followerCount));
}

void FormationController::_formationOffset(int followerIndex, int totalFollowers,
                                           double &distanceMeters, double &azimuthOffsetDeg, double &altitudeStepMeters) const
{
    const int rank = (followerIndex + 1) / 2;              // 排数（1 起）
    const int side = (followerIndex % 2 == 1) ? -1 : +1;   // 奇数槽位在左，偶数在右

    switch (_formationType) {
    case FormationWedge:   // 楔形：长机后方 ±30° 逐排展开，逐排升高
        distanceMeters      = _spacingMeters * rank;
        azimuthOffsetDeg    = 180.0 + side * 30.0;
        altitudeStepMeters  = _verticalStepMeters * rank;
        break;
    case FormationColumn:  // 纵队：长机正后方依次排开，逐机升高
        distanceMeters      = _spacingMeters * followerIndex;
        azimuthOffsetDeg    = 180.0;
        altitudeStepMeters  = _verticalStepMeters * followerIndex;
        break;
    case FormationLine:    // 横队：长机左右两翼展开，同高度
        distanceMeters      = _spacingMeters * rank;
        azimuthOffsetDeg    = side * 90.0;
        altitudeStepMeters  = 0.0;
        break;
    case FormationCircle:  // 环形：僚机均布在长机周围，同高度
        distanceMeters      = _spacingMeters;
        azimuthOffsetDeg    = (totalFollowers > 0) ? (360.0 * (followerIndex - 1) / totalFollowers) : 0.0;
        altitudeStepMeters  = 0.0;
        break;
    }
}

void FormationController::_setStatusText(const QString &text)
{
    if (_statusText != text) {
        _statusText = text;
        emit statusTextChanged();
    }
}
