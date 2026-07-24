/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmConstraintChecker（WP2.1 约束校验引擎 v1）
 *
 * 统一校验闸门的第二/三道关卡（语义与空域/能量约束），在第一道
 * （Schema+白名单+限幅，SwarmManager::parseAndValidate）之后执行：
 *   1. 地理围栏/空域：include 多边形外或 exclude 多边形内的作业区 → error
 *   2. 航程-电量：覆盖任务估算用时 vs 剩余电量（含安全余量）→ error/warning
 *   3. 机间间隔与去冲突：编队间距下限、多作业区重叠 → error/warning
 *   4. 通信链路覆盖：作业点距集结点超链路半径 → warning/error
 *   5. 载荷/规模匹配（模板级）：min_fleet 不足 → error；载荷未知 → warning
 *
 * 违例项均带"修改建议"，供面板展示与 LLM 修正参考。
 * 仅依赖 QtCore + SwarmAction/SwarmTypes，可脱离 QGC 独立单元测试。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QPair>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include "SwarmAction.h"
#include "SwarmTypes.h"

class SwarmConstraintChecker
{
public:
    struct Violation {
        QString severity;      ///< "error" / "warning"
        QString rule;          ///< 规则标识（fence/battery/spacing/deconflict/comm/fleet/payload）
        QString message;       ///< 违例描述
        QString suggestion;    ///< 修改建议
    };

    struct Config {
        QList<QList<QPair<double, double>>> includePolygons;   ///< 允许空域（空=不限制）
        QList<QList<QPair<double, double>>> excludePolygons;   ///< 禁飞区
        double maxAltitudeM   = 0.0;    ///< 围栏高度上限（0=不限制）
        double commRangeM     = 1500.0; ///< 链路覆盖半径
        double homeLat        = 0.0;    ///< 集结/指控点（无效坐标时用地图中心替代）
        double homeLon        = 0.0;
        double cruiseSpeedMs  = 12.0;   ///< 巡航速度（估算用）
        double climbRateMs    = 2.5;    ///< 爬升速率
        double enduranceMin   = 25.0;   ///< 满电续航（估算用）
        double reserveFactor  = 0.8;    ///< 可用电量安全系数（保留 20%）
        double minSeparationM = 3.0;    ///< 编队最小间距
    };

    /// 从 JSON 加载配置（fences.json）；缺失字段用默认值
    static Config configFromJson(const QJsonObject &obj);
    static QJsonObject configToJson(const Config &cfg);

    /// 对动作序列做完整约束检查（mapCenter 用于无绝对坐标动作的参照）
    QList<Violation> check(const QList<SwarmAction> &plan,
                           const QList<SwarmTypes::VehicleState> &vehicles,
                           const Config &cfg,
                           double mapCenterLat, double mapCenterLon) const;

    /// 模板级检查：规模与载荷（requiredPayload 空串=无要求；fleetPayloadKnown=false=载荷能力未知）
    QList<Violation> checkTemplate(int minFleet, const QString &requiredPayload,
                                   int onlineCount, bool fleetPayloadKnown) const;

    /// 是否有 error 级违例
    static bool hasError(const QList<Violation> &violations);

    /// 覆盖任务估算（米/分钟）：条带数、单架次扫掠距离
    static void coverageGeometry(double lengthMeters, double widthMeters, double spacingMeters,
                                 int &bands, double &sweepDistanceM);

private:
    struct CoverageArea { double lat = 0.0; double lon = 0.0; double lengthM = 0.0; double widthM = 0.0; };

    void _checkFence(const SwarmAction &action, const Config &cfg,
                     double mapCenterLat, double mapCenterLon, QList<Violation> &out) const;
    void _checkBattery(const SwarmAction &action, const QList<SwarmTypes::VehicleState> &vehicles,
                       const Config &cfg, double mapCenterLat, double mapCenterLon,
                       QList<Violation> &out) const;
    void _checkTakeoffAlt(const SwarmAction &action, const Config &cfg, QList<Violation> &out) const;
    void _checkComm(const SwarmAction &action, const Config &cfg,
                    double mapCenterLat, double mapCenterLon, QList<Violation> &out) const;
    void _checkSpacing(const SwarmAction &action, const Config &cfg, QList<Violation> &out) const;
    void _checkDeconflict(const QList<CoverageArea> &areas, QList<Violation> &out) const;
};
