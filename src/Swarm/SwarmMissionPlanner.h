/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmMissionPlanner
 *
 * 集群任务规划器（与人工 Plan 视图同一套任务管线）：
 *   - 输入一个矩形作业区域（中心点 + 长/宽 + 旋转角）与航线参数（高度、行距）
 *   - 将区域按“垂直于航线方向”均匀切分为 N 个条带（N = 目标车辆数）
 *   - 每个条带内生成割草机（boustrophedon）往返航线，自动分配给一架无人机
 *   - 用与 MissionController 完全相同的 MissionItem 原语组装任务，
 *     经同一个 PlanManager::writeMissionItems 通道上传（起飞-航点-返航）；
 *     上传成功后回读同步，使 QGC Plan 视图显示并允许人工编辑同一份任务
 *   - 任务进度监控（SwarmTaskController）对 AI 上传与人工上传一视同仁
 *
 * 扩展方向（见设计文档）：多边形区域、按续航能力加权分区、在线重规划。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QObject>
#include <QtCore/QVariantMap>
#include <QtPositioning/QGeoCoordinate>
#include <QtQmlIntegration/QtQmlIntegration>
#include <functional>

class MissionItem;
class Vehicle;

class SwarmMissionPlanner : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("")

    Q_PROPERTY(double surveyAltitude READ surveyAltitude WRITE setSurveyAltitude NOTIFY surveyAltitudeChanged)
    Q_PROPERTY(double surveySpacing READ surveySpacing WRITE setSurveySpacing NOTIFY surveySpacingChanged)
    Q_PROPERTY(int lastPlanVehicleCount READ lastPlanVehicleCount NOTIFY planChanged)
    Q_PROPERTY(int lastPlanWaypointCount READ lastPlanWaypointCount NOTIFY planChanged)
    /// 各机当前机载任务项数（vehicleId → itemCount，QString 键），AI 上传后回读刷新
    Q_PROPERTY(QVariantMap missionStatus READ missionStatus NOTIFY missionStatusChanged)

public:
    explicit SwarmMissionPlanner(QObject* parent = nullptr);
    ~SwarmMissionPlanner();

    double surveyAltitude() const { return _surveyAltitude; }

    void setSurveyAltitude(double altitude);

    double surveySpacing() const { return _surveySpacing; }

    void setSurveySpacing(double spacing);

    int lastPlanVehicleCount() const { return _lastPlanVehicleCount; }

    int lastPlanWaypointCount() const { return _lastPlanWaypointCount; }

    /// 生成区域覆盖航线并上传至全部目标车辆（选中集合优先，否则全体在线车辆）。
    /// @param center          区域中心
    /// @param lengthMeters    区域沿航线方向长度（米）
    /// @param widthMeters     区域垂直航线方向宽度（米）
    /// @param angleDeg        航线方向方位角（度，0=北，90=东）
    /// @param assignMode      条带分配方式：sequential（id 升序，默认）/ auction（集中式拍卖，WP2.4）/
    /// cbba（分布式一致性拍卖）
    Q_INVOKABLE bool planAndUploadCoverage(const QGeoCoordinate& center, double lengthMeters, double widthMeters,
                                           double angleDeg, const QString& assignMode = QStringLiteral("sequential"));

    /// 清空全部目标车辆上的任务
    Q_INVOKABLE void clearAllMissions();

    /// 构建单架机的区域覆盖任务项序列（起飞→航点→返航）。
    /// 与上传逻辑分离，供预览/测试/其他调用方复用；调用方负责释放返回的 MissionItem。
    QList<MissionItem*> buildCoverageMissionItems(const QGeoCoordinate& homeCoord, const QGeoCoordinate& center,
                                                  double lengthMeters, double widthMeters, double angleDeg,
                                                  double bandMin, double bandMax) const;

    QVariantMap missionStatus() const { return _missionStatus; }

    /// 设置逐机自动控制许可判定，阻止向无有效租约的车辆上传或清除任务。
    void setVehicleAutomationAllowedCallback(std::function<bool(int)> callback);

signals:
    void surveyAltitudeChanged();
    void surveySpacingChanged();
    void planChanged();
    void missionStatusChanged();
    /// 任务成功上传并回读同步（vehicleId，itemCount）
    void missionUploaded(int vehicleId, int itemCount);
    /// 规划/上传反馈（供界面显示）
    void plannerFeedback(const QString& message);

private:
    /// 本地平面坐标(x=沿航线, y=垂直航线, 单位米) → WGS84 坐标
    QGeoCoordinate _localToGeo(const QGeoCoordinate& center, double angleDeg, double x, double y) const;
    void _trackUpload(Vehicle* vehicle, int itemCount);

    double _surveyAltitude = 30.0;  ///< 作业高度（相对起飞点，米）
    double _surveySpacing = 20.0;   ///< 航线间隔（米）
    int _lastPlanVehicleCount = 0;
    int _lastPlanWaypointCount = 0;
    QVariantMap _missionStatus;  ///< vehicleId(QString) → 机载任务项数
    std::function<bool(int)> _vehicleAutomationAllowed;
};
