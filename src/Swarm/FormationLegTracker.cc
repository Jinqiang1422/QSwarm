#include "FormationLegTracker.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>
#include <algorithm>
#include <cmath>
#include <utility>

using FormationLegPlanner::LegMember;

void FormationLegTracker::start(FormationLegPlanner::Leg leg, qint64 nowMs)
{
    // leg 按值传入：调用方可以传 this->leg() 来重启当前航段；reset() 会清空 _leg，
    // 但形参 leg 在进入函数体前已经是独立拷贝，不会被 reset() 连带清空。
    reset();
    _leg = std::move(leg);
    _startMs = nowMs;
    _state = State::Running;
}

void FormationLegTracker::reset()
{
    *this = FormationLegTracker();
}

double FormationLegTracker::elapsedSec(qint64 nowMs) const
{
    return _state == State::Idle ? 0.0 : (nowMs - _startMs) / 1000.0;
}

void FormationLegTracker::markSent(int id, double speed)
{
    _sentSpeed.insert(id, speed);
}

void FormationLegTracker::removeMember(int id)
{
    _leg.members.removeIf([id](const LegMember& member) { return member.id == id; });
    _sentSpeed.remove(id);
    _progress.remove(id);
    _distanceToEnd.remove(id);
}

void FormationLegTracker::abort(const QString& reason)
{
    if (_state != State::Running)
        return;
    _state = State::Aborted;
    _reason = reason;
}

bool FormationLegTracker::_arrived(const LegMember& member, const QGeoCoordinate& position) const
{
    return position.distanceTo(member.end) <= kArrivalHorizontalMeters &&
           std::abs(position.altitude() - member.end.altitude()) <= kArrivalVerticalMeters;
}

double FormationLegTracker::_speedCap(double baseSpeed, double maxSpeed) const
{
    if (std::isfinite(maxSpeed) && maxSpeed > 0)
        return std::max(baseSpeed, kMaxSpeedFactor * maxSpeed);
    return std::max(1.5 * baseSpeed, baseSpeed + 1.0);
}

QList<FormationLegTracker::Command> FormationLegTracker::update(const QHash<int, QGeoCoordinate>& positions,
                                                                const QSet<int>& pending, qint64 nowMs, double maxSpeed)
{
    if (_state != State::Running)
        return {};

    // 实测定义统一：坐标有效且高度为有限值；三处（共识/偏离、超时未到位、调速指令）都按同一定义判断，
    // 缺失或异常时既不参与共识与偏离，也不下发指令（含首次下发），只记为未到位，交由超时兜底。
    const auto isMeasured = [](const QGeoCoordinate& position) {
        return position.isValid() && std::isfinite(position.altitude());
    };
    QSet<int> measured;
    for (const LegMember& member : std::as_const(_leg.members)) {
        if (isMeasured(positions.value(member.id)))
            measured.insert(member.id);
    }

    // 进度：实测位置在起点→终点连线上的投影比例；水平航程可忽略的成员不参与共识
    double progressSum = 0;
    int moving = 0;
    QStringList late;
    for (const LegMember& member : std::as_const(_leg.members)) {
        if (!measured.contains(member.id)) {
            late.append(QString::number(member.id));
            continue;
        }
        const QGeoCoordinate position = positions.value(member.id);
        _distanceToEnd.insert(member.id, position.distanceTo(member.end));
        if (!_arrived(member, position))
            late.append(QString::number(member.id));
        if (member.distance < FormationLegPlanner::kNegligibleDistance) {
            _progress.insert(member.id, 1.0);
            continue;
        }
        const auto path = FormationLegPlanner::localOffset(member.start, member.end);
        const auto actual = FormationLegPlanner::localOffset(member.start, position);
        const double s = std::clamp(
            (actual.east * path.east + actual.north * path.north) / (member.distance * member.distance), 0.0, 1.0);
        _progress.insert(member.id, s);
        progressSum += s;
        ++moving;
    }
    _consensus = moving > 0 ? progressSum / moving : 1.0;

    // 偏离：实测位置与「共识进度处的同步理想位置」的水平距离
    _deviation = 0;
    for (const LegMember& member : std::as_const(_leg.members)) {
        if (member.distance < FormationLegPlanner::kNegligibleDistance || !measured.contains(member.id))
            continue;
        const QGeoCoordinate position = positions.value(member.id);
        const auto path = FormationLegPlanner::localOffset(member.start, member.end);
        const auto actual = FormationLegPlanner::localOffset(member.start, position);
        _deviation = std::max(_deviation,
                              std::hypot(actual.east - _consensus * path.east, actual.north - _consensus * path.north));
    }
    _maxDeviation = std::max(_maxDeviation, _deviation);

    if (late.isEmpty()) {
        _state = State::Completed;
        return {};
    }
    if (_deviation > kMaxDeviationMeters) {
        _state = State::Aborted;
        _reason = QCoreApplication::translate("FormationLegTracker", "编队偏离同步位置 %1 米，航段已终止")
                      .arg(_deviation, 0, 'f', 1);
        return {};
    }
    if (elapsedSec(nowMs) > 2.0 * _leg.durationSec + kTimeoutSlackSec) {
        _state = State::TimedOut;
        _reason = QCoreApplication::translate("FormationLegTracker", "航段超时：%1 号机未到位")
                      .arg(late.join(QStringLiteral("、")));
        return {};
    }

    QList<Command> commands;
    for (const LegMember& member : std::as_const(_leg.members)) {
        if (pending.contains(member.id) || !measured.contains(member.id))
            continue;
        const QGeoCoordinate position = positions.value(member.id);
        if (!_sentSpeed.contains(member.id)) {
            commands.append({member.id, member.end, member.speed});
            continue;
        }
        if (member.speed <= 0 || _arrived(member, position))
            continue;
        const double lag = (_consensus - _progress.value(member.id)) * member.distance;
        const double speed = std::clamp(member.speed + kSpeedGain * lag, kMinSpeedFactor * member.speed,
                                        _speedCap(member.speed, maxSpeed));
        if (std::abs(speed - _sentSpeed.value(member.id)) >= kResendThreshold)
            commands.append({member.id, member.end, speed});
    }
    return commands;
}
