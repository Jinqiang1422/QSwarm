/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmManager
 *
 * 基于 MultiVehicleManager 的集群批量指令管理器，同时也是集群能力的统一枢纽：
 *   - 多机实时监控数据出口（vehicles / selectedVehicles 直接复用 MultiVehicleManager 模型）
 *   - 集群一键指令：解锁/上锁、起飞、降落、返航、暂停、开始任务、紧急上锁
 *   - 统一动作管线：parseAndValidate()（白名单、参数、目标严格校验）与 executeActions()，
 *     供大模型（SwarmAIAgent）、任务状态机（SwarmTaskController）与人工界面共用
 *   - 子控制器：编队（FormationController）、任务规划（SwarmMissionPlanner）、
 *     大模型接口（SwarmAIAgent）、任务状态机（SwarmTaskController）
 *
 * 设计说明：
 *   - 与 QGC master(v5.x) 架构一致：Q_APPLICATION_STATIC 单例 + QML_ELEMENT，
 *     在 QGCApplication 初始化阶段调用 SwarmManager::instance()->init()。
 *   - 所有批量指令按 commandIntervalMs 毫秒间隔“交错”下发，避免 MAVLink 链路瞬时拥塞，
 *     也避免多机在同一拍同时响应（如同时离地导致的气流干扰）。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QObject>
#include <QtPositioning/QGeoCoordinate>
#include <QtQmlIntegration/QtQmlIntegration>
#include <functional>
#include <memory>
#include <optional>

#include "SwarmAction.h"
#include "SwarmAutonomy.h"
#include "SwarmConstraintChecker.h"
#include "SwarmControlLease.h"
#include "SwarmEventDetector.h"
#include "SwarmTrackFusion.h"
#include "SwarmTypes.h"
#include "SwarmVideoHub.h"

class FormationController;
class SwarmMissionPlanner;
class SwarmAIAgent;
class SwarmCOPModel;
class SwarmPerceptionBridge;
class SwarmTaskController;
class SwarmTaskLibrary;
class QTimer;
class Vehicle;
class QmlObjectListModel;
#ifdef QGC_UNITTEST_BUILD
class SwarmAcceptanceServer;
#endif

class SwarmManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("")
    Q_MOC_INCLUDE("Vehicle.h")
    Q_MOC_INCLUDE("QmlObjectListModel.h")
    Q_MOC_INCLUDE("FormationController.h")
    Q_MOC_INCLUDE("SwarmMissionPlanner.h")
    Q_MOC_INCLUDE("SwarmAIAgent.h")
    Q_MOC_INCLUDE("SwarmTaskController.h")
    Q_MOC_INCLUDE("SwarmTaskLibrary.h")

    Q_PROPERTY(QmlObjectListModel* vehicles READ vehicles CONSTANT)
    Q_PROPERTY(QmlObjectListModel* selectedVehicles READ selectedVehicles CONSTANT)
    Q_PROPERTY(int vehicleCount READ vehicleCount NOTIFY vehicleCountChanged)
    Q_PROPERTY(int commandIntervalMs READ commandIntervalMs WRITE setCommandIntervalMs NOTIFY commandIntervalMsChanged)
    Q_PROPERTY(FormationController* formation READ formation CONSTANT)
    Q_PROPERTY(SwarmMissionPlanner* missionPlanner READ missionPlanner CONSTANT)
    Q_PROPERTY(SwarmAIAgent* aiAgent READ aiAgent CONSTANT)
    Q_PROPERTY(SwarmTaskController* taskController READ taskController CONSTANT)
    Q_PROPERTY(SwarmTaskLibrary* taskLibrary READ taskLibrary CONSTANT)
    Q_PROPERTY(int autonomyLevel READ autonomyLevel WRITE setAutonomyLevel NOTIFY autonomyLevelChanged)
    Q_PROPERTY(QStringList l3AllowedTypes READ l3AllowedTypes WRITE setL3AllowedTypes NOTIFY l3AllowedTypesChanged)
    Q_PROPERTY(QString auditDir READ auditDir CONSTANT)
    Q_PROPERTY(SwarmCOPModel* copModel READ copModel CONSTANT)
    Q_PROPERTY(SwarmPerceptionBridge* perception READ perception CONSTANT)
    Q_PROPERTY(QVariantList alerts READ alerts NOTIFY alertsChanged)
    Q_PROPERTY(QString situationOneLiner READ situationOneLiner NOTIFY situationChanged)
    Q_PROPERTY(QVariantMap executionStates READ executionStates NOTIFY executionStatesChanged)
    Q_PROPERTY(QString executionMessage READ executionMessage NOTIFY executionStatesChanged)
    Q_PROPERTY(quint64 lastExecutionId READ lastExecutionId NOTIFY executionStatesChanged)
    Q_MOC_INCLUDE("SwarmCOPModel.h")
    Q_MOC_INCLUDE("SwarmPerceptionBridge.h")

public:
    enum class ExecutionApproval
    {
        Automatic,
        OperatorConfirmed
    };

    explicit SwarmManager(QObject* parent = nullptr);
    ~SwarmManager();

    static SwarmManager* instance();
    void init();
    void shutdown();

    QmlObjectListModel* vehicles() const;
    QmlObjectListModel* selectedVehicles() const;
    int vehicleCount() const;
    QList<int> boundTargetIds() const;

    int commandIntervalMs() const { return _commandIntervalMs; }

    void setCommandIntervalMs(int intervalMs);

    FormationController* formation() const { return _formation; }

    SwarmMissionPlanner* missionPlanner() const { return _missionPlanner; }

    SwarmAIAgent* aiAgent() const { return _aiAgent; }

    SwarmTaskController* taskController() const { return _taskController; }

    SwarmTaskLibrary* taskLibrary() const { return _taskLibrary; }

    int autonomyLevel() const { return _autonomyLevel; }

    void setAutonomyLevel(int level);

    QStringList l3AllowedTypes() const { return _l3AllowedTypes; }

    void setL3AllowedTypes(const QStringList& types);
    QString auditDir() const;

    SwarmCOPModel* copModel() const { return _copModel; }

    SwarmPerceptionBridge* perception() const { return _perception; }

    QVariantList alerts() const { return _alertsVariant; }

    QString situationOneLiner() const { return _situationOneLiner; }

    // ------------------------------------------------------------------
    // P3 感知融合与态势理解
    // ------------------------------------------------------------------

    /// 启动/停止感知链路接收（UDP/TUNNEL 字节流，默认 15600 端口）
    Q_INVOKABLE bool startPerception(int port = 15600);
    Q_INVOKABLE void stopPerception();

    /// 结构化态势摘要 JSON 文本（WP3.5，供 AIAgent/面板）
    Q_INVOKABLE QString situationDigestJson() const;

    /// 航迹融合引擎（事件检测/单测直接驱动）
    SwarmTrackFusion& trackFusion() { return _fusion; }

    /// 视频汇聚（通道注册/坐标解算，C++ 侧）
    SwarmVideoHub& videoHub() { return _videoHub; }

    /// 自主等级信息（供 QML 显示）
    Q_INVOKABLE QString autonomyLevelName() const;
    Q_INVOKABLE QString autonomyLevelDescription() const;

    /// 审计接口（供 QML）：会话 ID / 事件数 / 导出当前会话（返回导出路径，失败为空串）
    Q_INVOKABLE QString auditSessionId() const;
    Q_INVOKABLE int auditEventCount() const;
    Q_INVOKABLE QString exportAudit();

    /// 文本安全检查（防注入过滤，供 QML 预检）：返回 {level, matched, score} JSON 字符串
    Q_INVOKABLE QString checkTextSafety(const QString& text) const;

#ifdef QGC_UNITTEST_BUILD
    QString controlLeaseState(int vehicleId) const;
    bool automationAllowed(int vehicleId) const;
#endif

    // ------------------------------------------------------------------
    // 自主等级闸门（WP1.3）：AI 侧执行路径必过；操作员手动直控不受限
    // ------------------------------------------------------------------

    /// 当前等级是否整体禁止 AI 侧执行（仅 L1）。reason 输出原因。
    bool aiExecutionBlocked(QString* reason = nullptr) const;

    /// 判定动作序列在当前等级下是否需要人工确认挂起（L2 含危险即需确认；L3 按白名单）。
    bool needsConfirmation(const QList<SwarmAction>& actions, QString* reason = nullptr) const;

    // ------------------------------------------------------------------
    // 统一校验闸门第二/三道（WP2.1 约束引擎）与解析式预演（WP2.7）
    // ------------------------------------------------------------------

    /// 约束校验：返回违例项列表（QVariantList of {severity, rule, message, suggestion}），并写审计
    QVariantList checkConstraints(const QList<SwarmAction>& plan, double mapLat, double mapLon);
    /// 用实时机况重校验计划是否含 error 级约束违例(供状态机推进前的重校验)
    bool planHasConstraintError(const QList<SwarmAction>& actions) const;

    /// 解析式仿真预演：估算 {duration_min, distance_km, battery_margin_pct, summary, notes}
    QVariantMap estimatePlan(const QList<SwarmAction>& plan, double mapLat, double mapLon);

    /// 围栏/空域配置摘要（供 QML 显示）
    Q_INVOKABLE QString fenceSummary();

    /// 批量解锁(arm=true)/上锁。作用于选中车辆；无选中时作用于全部在线车辆。
    Q_INVOKABLE void armAll(bool arm);

    /// 批量起飞到相对高度 altitudeRelative（米）。
    /// 每架机在自己的错峰时隙重新确认解锁，再延迟起飞，避免等待期间触发 PX4 自动上锁。
    Q_INVOKABLE void takeoffAll(double altitudeRelative);

    /// 批量原地降落
    Q_INVOKABLE void landAll();

    /// 批量返航
    Q_INVOKABLE void rtlAll();

    /// 批量暂停（悬停/刹车，等效 QGC 单机 Pause）
    Q_INVOKABLE void pauseAll();

    /// 暂停任务已绑定的目标，不读取当前 UI 选择。
    void pauseTargets(const QList<int>& ids);
    /// Retire only these members from queued commands, tasks and persistent formation control.
    void cancelTargets(const QList<int>& ids, const QString& reason);
    void authorizeTaskTargets(const QList<int>& ids);

    /// 批量开始已上传的航线任务
    Q_INVOKABLE void startMissionAll();

    /// 紧急强制上锁（param2=21196 强制，含空中）。仅用于地面紧急处置，飞行中使用将导致坠机。
    Q_INVOKABLE void emergencyDisarmAll();

    /// 经控制租约闸门启动/停止持续编队控制。
    Q_INVOKABLE bool startFormation(int leaderVehicleId);
    Q_INVOKABLE void stopFormation();

    /// 经控制租约闸门生成上传区域覆盖任务，或清空已有任务。
    Q_INVOKABLE bool planAndUploadCoverage(const QGeoCoordinate& center, double lengthMeters, double widthMeters,
                                           double angleDeg, const QString& assignMode = QStringLiteral("sequential"));
    Q_INVOKABLE void clearAllMissions();

    // ------------------------------------------------------------------
    // 统一动作管线（大模型 / 任务状态机 / 人工界面共用）
    // ------------------------------------------------------------------

    /// 把动作 JSON 数组解析并校验为可执行动作序列：
    /// 白名单、目标与参数严格检查；任一非法项拒绝整计划并写入 warnings。
    QList<SwarmAction> parseAndValidate(const QJsonArray& actionsJson, const QGeoCoordinate& mapCenter,
                                        QStringList& warnings, const QList<int>& initialTargets = {}) const;

    /// 按序执行动作序列（映射到本类批量指令与各子控制器）。
    /// origin 标记来源（ai / ai_confirmed / task / template / operator），写入审计日志便于回放归因。
    bool executeActions(const QList<SwarmAction>& actions, const QString& origin = QStringLiteral("ai"),
                        ExecutionApproval approval = ExecutionApproval::Automatic);

    quint64 lastExecutionId() const { return _lastExecutionId; }

    QVariantMap executionStates() const { return _executionStates; }

    QString executionMessage() const { return _executionMessage; }

signals:
    void executionFinished(quint64 executionId, bool success, const QString& reason);
    void executionProgress(quint64 executionId, int vehicleId, const QString& state);
    void executionStatesChanged();
    void vehicleCountChanged();
    void commandIntervalMsChanged();
    void autonomyLevelChanged();
    void l3AllowedTypesChanged();
    void alertsChanged();
    void situationChanged();
    /// 集群指令执行反馈（供界面显示，例如“已向 3 架无人机下达起飞指令”）
    void swarmCommandFeedback(const QString& message);
    /// 新告警（WP3.6，分级推送；critical 同时触发 AI 处置建议）
    void swarmAlert(const QString& level, const QString& rule, const QString& message);

private slots:
    void _vehicleAdded(Vehicle* vehicle);
    void _vehicleRemoved(Vehicle* vehicle);
    void _onTrackBatch(const QVariantList& tracks, qint64 timeUsec);
    void _onPerceptionTick();

private:
    struct Execution;
    void _advanceExecution(const std::shared_ptr<Execution>& execution);
    void _finishExecution(const std::shared_ptr<Execution>& execution, bool success, const QString& reason);
    bool _executeImmediateActions(const QList<SwarmAction>& actions, const QString& origin, ExecutionApproval approval);
    QHash<quint64, std::shared_ptr<Execution>> _executions;
    quint64 _lastExecutionId = 0;
    QVariantMap _executionStates;
    QString _executionMessage;
    /// 当前指令作用对象：有选中车辆时返回选中集合，否则返回全部在线车辆（按系统 id 升序）
    QList<Vehicle*> _targetVehicles(bool requireActiveLease = false) const;

    /// 以 staggerMs 为间隔对目标车辆依次执行 action；自动化路径在定时器触发时再次校验租约。
    void _forEachVehicleStaggered(const QString& commandName, int staggerMs,
                                  const std::function<void(Vehicle*, bool)>& action);
    void _requestAutomationLeases(const QString& origin);
    void _cancelSelectedTargets(const QString& reason);
    bool _automationAllowedAtDispatch(int vehicleId, bool requireActiveLease, const QString& commandName);
    bool _formationAutomationAllowed(int vehicleId);
    QStringList _autonomousModes(Vehicle* vehicle) const;

    void _loadSettings();

    /// 目标机状态快照（选中集合优先，否则全体在线），供约束引擎/估算器
    QList<SwarmTypes::VehicleState> _vehicleStates(const std::optional<QList<int>>& ids = std::nullopt) const;
    QList<SwarmConstraintChecker::Violation> _planViolations(const QList<SwarmAction>& actions, double mapLat,
                                                             double mapLon) const;
    /// plan_tactic 协同战术规划执行（跟踪/围捕/拦截/巡逻，输出方案经 swarmCommandFeedback）
    void _executeTactic(const QVariantMap& params);
    /// 加载围栏/空域配置（AppDataLocation/SwarmConfig/fences.json；缺失时写默认）
    SwarmConstraintChecker::Config _constraintConfig() const;

    int _commandIntervalMs = 300;  ///< 常规指令机间间隔
    int _takeoffStaggerMs = 2500;  ///< 起飞指令机间间隔（安全关键，默认更大）
    QHash<int, quint64> _commandGenerations;
    QSet<int> _automationExcludedIds;
    int _autonomyLevel = 2;       ///< 自主等级（1=L1 2=L2 3=L3），默认 L2 监督执行
    QStringList _l3AllowedTypes;  ///< L3 限定域自主白名单（动作类型）
    SwarmControlLease _lease;     ///< per-vehicle 控制租约（人工接管即跳过该机自动下发）
    bool _automationDispatchInProgress = false;
    std::optional<QList<int>> _dispatchTargetIds;
    QSet<int> _formationTargetIds;
    bool _formationAutomated = false;
    bool _shutdownComplete = false;
    FormationController* _formation = nullptr;
    SwarmMissionPlanner* _missionPlanner = nullptr;
    SwarmAIAgent* _aiAgent = nullptr;
    SwarmTaskController* _taskController = nullptr;
    SwarmTaskLibrary* _taskLibrary = nullptr;

    // ---- P3 感知融合与态势理解 ----
    SwarmCOPModel* _copModel = nullptr;
    SwarmPerceptionBridge* _perception = nullptr;
    SwarmTrackFusion _fusion;                  ///< 多源航迹融合（WP3.3）
    SwarmEventDetector _eventDetector;         ///< 异常事件规则引擎（WP3.6）
    SwarmVideoHub _videoHub;                   ///< 视频汇聚（WP3.1）
    QTimer* _perceptionTimer = nullptr;        ///< 2Hz 融合/事件/态势节拍
    QList<SwarmEventDetector::Alert> _alerts;  ///< 近期告警（上限 20 条）
    QVariantList _alertsVariant;
    QString _situationOneLiner;

#ifdef QGC_UNITTEST_BUILD
    SwarmAcceptanceServer* _acceptanceServer = nullptr;
#endif

    void _raiseAlerts(const QList<SwarmEventDetector::Alert>& alerts);
};
