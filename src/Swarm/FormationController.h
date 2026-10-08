/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — FormationController
 *
 * 长机-僚机（Leader-Follower）编队控制器：
 *   - 队形：楔形 / 纵队 / 横队 / 环形
 *   - 保持阶段（StationKeeping）：每个控制周期按长机位置与编队朝向计算各僚机槽位，
 *     通过 guidedModeGotoLocation（MAV_CMD_DO_REPOSITION）下发。
 *   - 航段阶段（Transit）：同步编队航段——全队（含长机）同时受令、速度与航程成比例、同时到达，
 *     每拍按进度共识调速（FormationLegPlanner / FormationLegTracker）。地图 goto、编队航线、
 *     启动与变队形都走航段。设计见 swarm_ext/docs/specs/2026-10-05-synchronized-formation-legs-design.md。
 *   - 编队朝向：长机水平速度超过 1 m/s（由其他方式驱动）时跟随长机航向，否则保持上一航段结束时的朝向。
 *   - 槽位分配：首次按 id 升序；航段规划按最小总位移重新分配（代价接近时保持原分配）。
 *
 * 安全设计：
 *   - 楔形/纵队默认带高度分层（verticalStepMeters），避免僚机处于长机下洗流中；
 *   - 航段规划逐对校验同步直线运动下的最小间距，不满足时拆段或垂直分层，仍不满足则拒绝；
 *   - 成员退出时尝试长机接替；无合格长机时阻塞引导并通知界面；
 *   - 仅对已解锁且已离地的僚机下发位置指令；goto/航线航段中长机也须持有自动控制租约；
 *   - 处于失效保护（HEARTBEAT system_status ≥ MAV_STATE_CRITICAL）的成员不再受令：僚机视同离队，
 *     长机则终止航段并阻塞编队。残留窗口：PX4 以 1 Hz 发送 HEARTBEAT，失效保护开始后最长约 1 秒
 *     （另加链路延迟）控制器看不到该状态，这期间下发的保持指令仍可能落入 PX4 的失效保护延迟窗口。
 *     该窗口在本功能之前就存在（保持阶段一直逐拍重发指令），这里只是缩小、并未消除。
 *
 * 说明：1 Hz 的 DO_REPOSITION 引导适用于 SITL 验证与低速编队；
 *       需要更平滑/高速的编队时，可改用 OFFBOARD + SET_POSITION_TARGET_GLOBAL_INT
 *       高频设定点流（见设计文档扩展路线）。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtCore/QVariantMap>
#include <QtPositioning/QGeoCoordinate>
#include <QtQmlIntegration/QtQmlIntegration>
#include <functional>

#include "FormationLegPlanner.h"
#include "FormationLegTracker.h"

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
    Q_PROPERTY(bool transitActive READ transitActive NOTIFY transitChanged)

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

    /// 一次航段规划使用的约束：同一份约束配置快照下的最小间距与逐机路径检查
    struct TransitConstraints
    {
        double minSeparation = 3.0;  ///< 最小间距（米）
        /// 路径区域检查：允许时返回空串，否则返回原因；为空表示不限制
        std::function<QString(int, const QGeoCoordinate&, const QGeoCoordinate&)> pathCheck;
    };

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

    bool transitActive() const { return _legCount > 0; }

    int transitLegIndex() const { return _legIndex; }

    int transitLegCount() const { return _legCount; }

    double formationHeading() const { return _formationHeading; }

    /// 编队是否正在引导该机：编队运行中的长机，或当前合格（已解锁、已离地、无失效保护、有自动控制许可）的僚机
    bool guides(int vehicleId) const;

    /// 发起 goto/航线航段的长机级前置条件（编队运行、长机在队且状态正常、无失效保护、有自动控制许可）；
    /// 满足时返回空串
    QString transitBlockedReason() const;

    /// 以 leaderVehicleId 为长机开始编队。长机由人工/任务驾驶，僚机自动跟随队形。
    bool start(int leaderVehicleId);

    /// 编队整体飞往 coordinate：长机终点为该点，高度保持长机当前高度。失败时 reason 给出原因。
    bool gotoFormation(const QGeoCoordinate& coordinate, QString* reason = nullptr);

    /// 依次飞经 waypoints（长机终点，含 AMSL 高度），每个航点全队对齐后再飞下一段。
    bool startRoute(const QList<QGeoCoordinate>& waypoints, QString* reason = nullptr);

    /// 只规划不执行，供确认框展示航点数与预计时长
    FormationLegPlanner::Plan planRoute(const QList<QGeoCoordinate>& waypoints) const;

    /// 航段状态（阶段、序号、各机起终点与进度、偏离、上次结果），供验收接口
    QVariantMap transitStatus() const;

    /// 僚机 id → 槽位编号（1 起）
    QHash<int, int> slotAssignment() const;

    /// 推进单架僚机的引导目标。目标先随槽位目的地整体平移（跟随长机运动，不限速），
    /// 再朝目的地做限速追赶：限速只约束追赶，避免单拍大幅跳变或时钟跳变放大指令。
    /// previousDestination 无效（首拍）时从 current 起步。
    static QGeoCoordinate advanceGuidanceTarget(const QGeoCoordinate& current,
                                                const QGeoCoordinate& previousDestination,
                                                const QGeoCoordinate& destination, double dtSeconds);

    /// 停止自动编队引导；不改变各机飞行模式
    Q_INVOKABLE void stop();

    /// 设置逐机自动控制许可判定。SwarmManager 集成时采用 fail-closed 租约判定。
    void setVehicleAutomationAllowedCallback(std::function<bool(int)> callback);
    void setTargetAllowedCallback(std::function<bool(int, const QGeoCoordinate&)> callback);
    /// 航段规划约束，每次规划调用一次；未设置时用默认最小间距且不检查路径区域
    void setTransitConstraintsCallback(std::function<TransitConstraints()> callback);
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
    void transitChanged();
    /// 航段或航线结束；success 为 false 时 reason 说明原因；kind 为 goto / route / reshape。
    /// 被新航段替换时不发出（新航段的结果会另行发出）。
    void transitFinished(bool success, const QString& reason, const QString& kind);

private slots:
    void _updateFormation();

private:
    struct Slot
    {
        double distance = 0;
        double bearing = 0;
        double altitude = 0;
        int index = 0;  ///< 槽位编号（0 起）
    };

    struct GuidanceTarget
    {
        QGeoCoordinate coordinate;
        qint64 updatedMs = 0;
        QGeoCoordinate destination;  ///< 上一拍实际下发的槽位目的地；用于随长机运动平移目标
    };

    /// 计算第 followerIndex 架僚机（1 起）的队形偏移
    /// @param distanceMeters   相对长机的水平距离
    /// @param azimuthOffsetDeg 相对长机航向的方位角偏移（0=正前方, 90=右侧, 180=正后方）
    /// @param altitudeStepMeters 相对长机的高度增量
    void _formationOffset(int followerIndex, int totalFollowers, double& distanceMeters, double& azimuthOffsetDeg,
                          double& altitudeStepMeters) const;

    void _setStatusText(const QString& text);
    void _rebuildSlots();
    bool _handover();
    void _onShapeChanged();
    void _block(const QString& text);
    /// 成员是否处于失效保护（HEARTBEAT system_status ≥ MAV_STATE_CRITICAL）。PX4 把每条带 CHANGE_MODE 的
    /// DO_REPOSITION 当作一次模式切换请求，在失效保护的悬停延迟窗口（COM_FAIL_ACT_T）内收到会被视为
    /// 用户接管而取消返航/降落，所以失效保护中的成员不得再收到任何编队指令。
    /// 只能看到最近一次 HEARTBEAT：PX4 以 1 Hz 发送，失效保护开始后最长约 1 秒（另加链路延迟）内仍判为
    /// 正常，该窗口只是缩小、并未消除。
    static bool _inFailsafe(const Vehicle* vehicle);
    /// 发起 goto/航线航段的前置条件（编队状态、长机状态、失效保护、自动控制许可）；满足时返回空串
    QString _transitBlockedReason(Vehicle* leader) const;
    QList<Vehicle*> _eligibleFollowers() const;
    bool _leaderMoving(Vehicle* leader) const;
    double _maxHorizontalSpeed(const QList<Vehicle*>& vehicles) const;
    FormationLegPlanner::Request _legRequest(Vehicle* leader, const QList<Vehicle*>& followers,
                                             const QList<QGeoCoordinate>& waypoints, bool commandLeader) const;
    bool _planTransit(const QList<QGeoCoordinate>& waypoints, const QString& kind, QString* reason);
    void _beginTransit(const FormationLegPlanner::Plan& plan, const FormationLegPlanner::Request& request,
                       const QString& kind);
    bool _beginReshape(Vehicle* leader, const QList<Vehicle*>& followers);
    void _applyAssignment(const FormationLegPlanner::Plan& plan, const FormationLegPlanner::Request& request);
    void _startNextLeg();
    void _updateTransit();
    /// notify 为 false 时只记录审计与状态，不发出 transitFinished（航段被新航段替换时）
    void _finishTransit(bool success, const QString& reason, bool notify = true);

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
    std::function<TransitConstraints()> _transitConstraints;
    QSet<int> _members;
    bool _membersBound = false;

    QHash<int, Slot> _slots;
    QHash<int, GuidanceTarget> _guidanceTargets;
    // Limit target motion, independently of the aircraft's own velocity controller.
    static constexpr double _horizontalTargetSpeed = 2.0;  ///< m/s
    static constexpr double _verticalTargetSpeed = 1.0;    ///< m/s

    double _formationHeading = 0;                          ///< 编队朝向（度）
    bool _reshapePending = false;                          ///< 启动或队形参数变更后，下一拍尝试同步收敛
    FormationLegTracker _tracker;
    QList<FormationLegPlanner::Leg> _legQueue;             ///< 未完成的航段，首项为当前航段
    int _legIndex = 0;
    int _legCount = 0;
    double _transitDurationSec = 0;
    QString _transitKind;  ///< goto / route / reshape
    QVariantMap _lastTransitResult;
    QList<QMetaObject::Connection> _resultConnections;
    /// 每次航段结束递增，写入 last_result，使两次相同结果可区分
    int _transitResultSerial = 0;
    /// 本次航段/航线中航段指令已被接受的成员
    QSet<int> _transitAccepted;
    /// 指令被接受后已观测到处于 goto 模式的成员：此后模式一变，即视为操作员或任务接管，该机离开航段
    QSet<int> _gotoModeSeen;
    QSet<int> _awaitingAck;                       ///< 本航段已发出、尚未收到回执的成员
    QSet<int> _transitDropped;                    ///< 本次航段/航线中途被移出的成员：后续航段不再纳入，直到航段结束
    static constexpr double _transitSpeed = 5.0;  ///< 编队速度 V（m/s）
    static constexpr double _transitSpeedFactor = 0.8;  ///< V 不超过成员最大水平速度最小值的比例
    static constexpr double _leaderMovingSpeed = 1.0;   ///< 长机水平速度超过此值视为由其他方式驱动（m/s）
    /// 原地变队形期间长机相对航段参考点的水平漂移上限（米）：超过即说明长机已被其他方式驱动，
    /// 原地航段的终点不再对应长机周围的槽位，取消航段并交回保持阶段。
    static constexpr double _reshapeLeaderDriftMeters = 2.0;
};
