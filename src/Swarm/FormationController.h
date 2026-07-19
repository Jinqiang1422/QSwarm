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
 *   - 长机丢失、上锁或位置无效时自动停止编队，僚机原地悬停；
 *   - 仅对已解锁且已离地的僚机下发位置指令。
 *
 * 说明：1 Hz 的 DO_REPOSITION 引导适用于 SITL 验证与低速编队；
 *       需要更平滑/高速的编队时，可改用 OFFBOARD + SET_POSITION_TARGET_GLOBAL_INT
 *       高频设定点流（见设计文档扩展路线）。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtQmlIntegration/QtQmlIntegration>

class Vehicle;

class FormationController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("")

    Q_PROPERTY(FormationType    formationType       READ formationType      WRITE setFormationType      NOTIFY formationTypeChanged)
    Q_PROPERTY(double           spacingMeters       READ spacingMeters      WRITE setSpacingMeters      NOTIFY spacingMetersChanged)
    Q_PROPERTY(double           verticalStepMeters  READ verticalStepMeters WRITE setVerticalStepMeters NOTIFY verticalStepMetersChanged)
    Q_PROPERTY(bool             active              READ active                                         NOTIFY activeChanged)
    Q_PROPERTY(int              leaderVehicleId     READ leaderVehicleId                                NOTIFY leaderVehicleIdChanged)
    Q_PROPERTY(int              followerCount       READ followerCount                                  NOTIFY followerCountChanged)
    Q_PROPERTY(QString          statusText          READ statusText                                     NOTIFY statusTextChanged)

public:
    explicit FormationController(QObject *parent = nullptr);
    ~FormationController();

    enum FormationType {
        FormationWedge = 0,     ///< 楔形（V 字）
        FormationColumn,        ///< 纵队
        FormationLine,          ///< 横队
        FormationCircle         ///< 环形
    };
    Q_ENUM(FormationType)

    FormationType formationType() const { return _formationType; }
    void setFormationType(FormationType type);
    double spacingMeters() const       { return _spacingMeters; }
    void setSpacingMeters(double spacing);
    double verticalStepMeters() const  { return _verticalStepMeters; }
    void setVerticalStepMeters(double step);
    bool   active() const              { return _active; }
    int    leaderVehicleId() const     { return _leaderVehicleId; }
    int    followerCount() const       { return _followerCount; }
    QString statusText() const         { return _statusText; }

    /// 以 leaderVehicleId 为长机开始编队。长机由人工/任务驾驶，僚机自动跟随队形。
    Q_INVOKABLE bool start(int leaderVehicleId);

    /// 停止编队，僚机原地悬停
    Q_INVOKABLE void stop();

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
    void _formationOffset(int followerIndex, int totalFollowers,
                          double &distanceMeters, double &azimuthOffsetDeg, double &altitudeStepMeters) const;

    void _setStatusText(const QString &text);

    FormationType _formationType      = FormationWedge;
    double        _spacingMeters      = 10.0;
    double        _verticalStepMeters = 3.0;
    int           _updateIntervalMs   = 1000;       ///< 队形目标点更新周期
    double        _minAirborneAltMeters = 1.5;      ///< 判定僚机已离地的相对高度阈值

    QTimer        _updateTimer;
    bool          _active          = false;
    int           _leaderVehicleId = 0;
    int           _followerCount   = 0;
    QString       _statusText;
};
