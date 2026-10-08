#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtPositioning/QGeoCoordinate>

#include "FormationLegPlanner.h"

/// 执行一个同步编队航段：每拍按各机进度做共识调速，并判定完成、超时与中止（设计 3.3）。
/// 纯状态机，不依赖 Vehicle 与定时器；所有 nowMs 参数均须来自单调时钟 SwarmClock::nowMs()，不可用墙上时间。
class FormationLegTracker
{
public:
    enum class State
    {
        Idle,
        Running,
        Completed,
        TimedOut,
        Aborted
    };

    struct Command
    {
        int id = 0;
        QGeoCoordinate target;
        double speed = 0;  ///< 0 表示由飞控默认速度完成
    };

    void start(FormationLegPlanner::Leg leg, qint64 nowMs);
    void reset();

    /// 每拍调用。positions：成员实测位置（含 AMSL 高度）；pending：上一条指令仍待 ACK 的成员；
    /// maxSpeed：成员最大水平速度中的最小值，未知时传 NaN。返回本拍需要（重新）下发的指令。
    QList<Command> update(const QHash<int, QGeoCoordinate>& positions, const QSet<int>& pending, qint64 nowMs,
                          double maxSpeed);

    /// 指令确已下发后调用，记录下发速度
    void markSent(int id, double speed);
    void removeMember(int id);
    void abort(const QString& reason);

    State state() const { return _state; }

    QString reason() const { return _reason; }

    const FormationLegPlanner::Leg& leg() const { return _leg; }

    double elapsedSec(qint64 nowMs) const;

    double consensusProgress() const { return _consensus; }

    double deviationMeters() const { return _deviation; }        ///< 本拍偏离同步理想位置的水平距离最大值

    double maxDeviationMeters() const { return _maxDeviation; }  ///< 本段迄今偏离同步理想位置的水平距离最大值

    QHash<int, double> progress() const { return _progress; }

    QHash<int, double> distanceToEnd() const { return _distanceToEnd; }

    static constexpr double kArrivalHorizontalMeters = 1.0;
    static constexpr double kArrivalVerticalMeters = 1.0;
    static constexpr double kSpeedGain = 0.5;        ///< 每米掉队增加的速度（1/秒）
    static constexpr double kResendThreshold = 0.3;  ///< 速度变化达到此值才重发（米/秒）
    static constexpr double kMinSpeedFactor = 0.5;   ///< 超前者最低降至基准速度的比例
    static constexpr double kMaxSpeedFactor = 0.9;   ///< 速度上限占最大水平速度的比例
    static constexpr double kTimeoutSlackSec = 20.0;
    /// 失步上限（米）：任一成员偏离同步理想位置超过此值即中止航段（Aborted），交回保持阶段。
    /// 成对间距只对同步直线运动校验过，失步越大，校验结论越不成立。
    static constexpr double kMaxDeviationMeters = 5.0;

private:
    bool _arrived(const FormationLegPlanner::LegMember& member, const QGeoCoordinate& position) const;
    double _speedCap(double baseSpeed, double maxSpeed) const;

    FormationLegPlanner::Leg _leg;
    qint64 _startMs = 0;
    State _state = State::Idle;
    QString _reason;
    QHash<int, double> _sentSpeed;
    QHash<int, double> _progress;
    QHash<int, double> _distanceToEnd;
    double _consensus = 0;
    double _deviation = 0;
    double _maxDeviation = 0;
};
