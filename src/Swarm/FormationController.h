/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — FormationController
 *
 * 长机-僚机（Leader-Follower）编队控制器：
 *   - 队形：楔形 / 纵队 / 横队 / 环形
 *   - 地面站侧解算：每个控制周期读取长机位置与航向，计算各僚机的队形目标点，
 *     通过 guidedModeGotoLocation（MAV_CMD_DO_REPOSITION）下发。
 *   - 僚机按 id 升序分配队形槽位，保证队形分配稳定。
 *
 * 安全设计：
 *   - 楔形/纵队默认带高度分层（verticalStepMeters），避免僚机处于长机下洗流中；
 *   - 成员退出时尝试长机接替；无合格长机时阻塞引导并通知界面；
 *   - 仅对已解锁且已离地的僚机下发位置指令。
 *
 * 说明：1 Hz 的 DO_REPOSITION 引导适用于 SITL 验证与低速编队；
 *       需要更平滑/高速的编队时，可改用 OFFBOARD + SET_POSITION_TARGET_GLOBAL_INT
 *       高频设定点流（见设计文档扩展路线）。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtPositioning/QGeoCoordinate>
#include <QtQmlIntegration/QtQmlIntegration>
#include <functional>

class Vehicle;

class FormationController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("")

    Q_PROPERTY(FormationType formationType READ formationType WRITE setFormationType NOTIFY formationTypeChanged)
    Q_PROPERTY(double spacingMeters READ spacingMeters WRITE setSpacingMeters NOTIFY spacingMetersChanged)
    Q_PROPERTY(
        double verticalStepMeters READ verticalStepMeters WRITE setVerticalStepMeters NOTIFY verticalStepMetersChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(bool blocked READ blocked NOTIFY activeChanged)
    Q_PROPERTY(int leaderVehicleId READ leaderVehicleId NOTIFY leaderVehicleIdChanged)
    Q_PROPERTY(int followerCount READ followerCount NOTIFY followerCountChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)

public:
    explicit FormationController(QObject* parent = nullptr);
    ~FormationController();

    enum FormationType
    {
        FormationWedge = 0,  ///< 楔形（V 字）
        FormationColumn,     ///< 纵队
        FormationLine,       ///< 横队
        FormationCircle      ///< 环形
    };
    Q_ENUM(FormationType)

    FormationType formationType() const { return _formationType; }

    void setFormationType(FormationType type);

    double spacingMeters() const { return _spacingMeters; }

    void setSpacingMeters(double spacing);

    double verticalStepMeters() const { return _verticalStepMeters; }

    void setVerticalStepMeters(double step);

    bool active() const { return _active; }

    bool blocked() const { return _blocked; }

    int leaderVehicleId() const { return _leaderVehicleId; }

    int followerCount() const { return _followerCount; }

    QString statusText() const { return _statusText; }

    /// 以 leaderVehicleId 为长机开始编队。长机由人工/任务驾驶，僚机自动跟随队形。
    bool start(int leaderVehicleId);

    /// 推进单架僚机的引导目标。目标先随槽位目的地整体平移（跟随长机运动，不限速），
    /// 再朝目的地做限速追赶：限速只约束追赶，避免单拍大幅跳变或时钟跳变放大指令。
    /// previousDestination 无效（首拍）时从 current 起步。
    static QGeoCoordinate advanceGuidanceTarget(const QGeoCoordinate& current, const QGeoCoordinate& previousDestination,
                                                const QGeoCoordinate& destination, double dtSeconds);

    /// 停止自动编队引导；不改变各机飞行模式
    Q_INVOKABLE void stop();

    /// 设置逐机自动控制许可判定。SwarmManager 集成时采用 fail-closed 租约判定。
    void setVehicleAutomationAllowedCallback(std::function<bool(int)> callback);
    void setTargetAllowedCallback(std::function<bool(int, const QGeoCoordinate&)> callback);
    void setMembers(const QSet<int>& members);
    void removeMembers(const QSet<int>& members);

signals:
    void formationTypeChanged();
    void spacingMetersChanged();
    void verticalStepMetersChanged();
    void activeChanged();
    void leaderVehicleIdChanged();
    void followerCountChanged();
    void statusTextChanged();

private slots:
    void _updateFormation();

private:
    /// 计算第 followerIndex 架僚机（1 起）的队形偏移
    /// @param distanceMeters   相对长机的水平距离
    /// @param azimuthOffsetDeg 相对长机航向的方位角偏移（0=正前方, 90=右侧, 180=正后方）
    /// @param altitudeStepMeters 相对长机的高度增量
    void _formationOffset(int followerIndex, int totalFollowers, double& distanceMeters, double& azimuthOffsetDeg,
                          double& altitudeStepMeters) const;

    void _setStatusText(const QString& text);
    void _rebuildSlots();

    FormationType _formationType = FormationWedge;
    double _spacingMeters = 10.0;
    double _verticalStepMeters = 3.0;
    int _updateIntervalMs = 1000;        ///< 队形目标点更新周期
    double _minAirborneAltMeters = 1.5;  ///< 判定僚机已离地的相对高度阈值

    QTimer _updateTimer;
    bool _active = false;
    bool _blocked = false;
    int _leaderVehicleId = 0;
    int _followerCount = 0;
    QString _statusText;
    std::function<bool(int)> _vehicleAutomationAllowed;
    std::function<bool(int, const QGeoCoordinate&)> _targetAllowed;
    QSet<int> _members;

    struct Slot
    {
        double distance = 0;
        double bearing = 0;
        double altitude = 0;
    };

    QHash<int, Slot> _slots;

    struct GuidanceTarget
    {
        QGeoCoordinate coordinate;
        qint64 updatedMs = 0;
        QGeoCoordinate destination;  ///< 上一拍实际下发的槽位目的地；用于随长机运动平移目标
    };

    QHash<int, GuidanceTarget> _guidanceTargets;
    // Limit target motion, independently of the aircraft's own velocity controller.
    static constexpr double _horizontalTargetSpeed = 2.0;  ///< m/s
    static constexpr double _verticalTargetSpeed = 1.0;    ///< m/s
    bool _membersBound = false;
    bool _handover();
};
