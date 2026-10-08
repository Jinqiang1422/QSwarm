#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtPositioning/QGeoCoordinate>
#include <functional>

/// 同步编队航段规划：纯函数，不依赖 Vehicle 与定时器。
/// 设计见 swarm_ext/docs/specs/2026-10-05-synchronized-formation-legs-design.md 第 3.2 节。
namespace FormationLegPlanner {

/// 局部东-北-天坐标（米）
struct Vector3
{
    double east = 0;
    double north = 0;
    double up = 0;
};

/// point 相对 origin 的局部东北天偏移（水平用球面距离与方位角换算，适用于数公里范围）。
/// 调用方须保证坐标有效；无效坐标时水平分量为 0、高度分量可能为 NaN。
Vector3 localOffset(const QGeoCoordinate& origin, const QGeoCoordinate& point);

/// 同步直线运动下两机的最小间距：相对位置 r(s) = r0 + s·delta，s ∈ [0, 1]
double minimumSeparation(const Vector3& r0, const Vector3& delta);

/// 行数 ≤ 列数的最小总代价分配（匈牙利算法）：返回每行分到的列。输入非法时返回空。
QList<int> minimumCostAssignment(const QList<QList<double>>& cost);

/// 编队朝向：from→to 的水平距离不小于 spacing 时取其方位角，否则保持 currentHeadingDeg。结果归一到 [0, 360)。
/// 保持当前朝向时，currentHeadingDeg 为 NaN 则返回 NaN。
double legHeading(const QGeoCoordinate& from, const QGeoCoordinate& to, double currentHeadingDeg, double spacing);

inline constexpr double kTrackingMargin = 1.0;         ///< 间距判据中的跟踪余量（米）
inline constexpr double kVerticalSpeedEstimate = 1.0;  ///< 估算垂直耗时用的保守升降速率（米/秒）
inline constexpr double kNegligibleDistance = 0.5;     ///< 小于此水平航程的成员不做速度同步（米）
inline constexpr int kMaxSubLegs = 4;                  ///< 均分拆段的最大段数

/// 槽位几何，与 FormationController 的槽位一致：相对参考点的水平距离、相对编队朝向的方位偏移、高度增量
struct SlotGeometry
{
    double distance = 0;
    double bearing = 0;
    double altitude = 0;
};

/// 槽位位置：参考点沿（编队朝向 + 方位偏移）推移 distance，高度 = 参考点高度 + 高度增量
QGeoCoordinate slotPosition(const QGeoCoordinate& reference, double headingDeg, const SlotGeometry& slot);

struct Member
{
    int id = 0;
    QGeoCoordinate position;  ///< 含 AMSL 高度
};

struct LegMember
{
    int id = 0;
    QGeoCoordinate start;
    QGeoCoordinate end;
    double distance = 0;  ///< 水平航程（米）
    double speed = 0;     ///< 指令地速（米/秒）；0 表示水平航程可忽略，由飞控默认速度完成
};

struct Leg
{
    QList<LegMember> members;  ///< 受令成员；原地航段不含长机
    QGeoCoordinate reference;  ///< 本段目标位姿的参考点（长机终点；垂直分层的中间阶段长机高度可能与之不同）
    double headingDeg = 0;     ///< 本段结束时的编队朝向
    double durationSec = 0;    ///< 预计耗时：水平同步时长与垂直估算时长取大
};

struct Request
{
    QList<Member> members;               ///< 参与规划的成员当前位置（含长机）
    QList<Member> obstacles;             ///< 不参与规划的空中编队成员：作静止点参与每段间距校验
    int leaderId = 0;
    QList<SlotGeometry> slotGeometries;  ///< 全部槽位，下标即槽位编号（成员名避开 Qt 的 slots 关键字宏）
    QSet<int> reservedSlots;             ///< 被未参与成员占用、不可分配的槽位
    QHash<int, int> currentAssignment;   ///< 现有槽位持有者（含未参与成员）id → 槽位下标
    QGeoCoordinate startReference;       ///< 当前参考点（长机位置），含高度
    double startHeadingDeg = 0;          ///< 当前编队朝向
    QList<QGeoCoordinate> waypoints;     ///< 依次到达的参考点（长机终点），含 AMSL 高度
    double spacing = 10;                 ///< 编队间距：位移小于该值时不改朝向
    double speed = 5;                    ///< 编队速度 V（米/秒）
    double minSeparation = 3;            ///< 约束最小间距 D_min（米）
    bool commandLeader = true;           ///< false：原地航段，长机不受令，作为静止点参与间距校验；
                                         ///< 为 false 时 waypoints 必须是长机当前位置（原地航段）
    /// 路径区域检查（电子围栏、高度上下限）：允许时返回空串，否则返回原因；为空表示不限制
    std::function<QString(int, const QGeoCoordinate&, const QGeoCoordinate&)> pathCheck;
};

struct Plan
{
    QList<Leg> legs;
    QHash<int, int> assignment;  ///< 参与规划的僚机 id → 槽位下标
    QString error;               ///< 非空表示规划失败，内容可直接展示给操作员

    bool ok() const { return error.isEmpty() && !legs.isEmpty(); }

    double durationSec() const;
};

/// 生成航段计划：每个航点一段；成对间距不满足时依次尝试均分为 2～kMaxSubLegs 小段与垂直分层三段（设计 3.2）。
Plan plan(const Request& request);

}  // namespace FormationLegPlanner
