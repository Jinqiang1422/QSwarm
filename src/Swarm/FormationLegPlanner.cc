#include "FormationLegPlanner.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QtMath>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace FormationLegPlanner {

namespace {

double dot(const Vector3& a, const Vector3& b)
{
    return a.east * b.east + a.north * b.north + a.up * b.up;
}

double normalizedHeading(double headingDeg)
{
    double wrapped = std::fmod(headingDeg, 360.0);
    if (wrapped < 0)
        wrapped += 360.0;
    // 浮点舍入可能让 360.0 - ε 直接舍到 360.0，这里收口到合法的 [0, 360) 区间。
    return wrapped >= 360.0 ? 0.0 : wrapped;
}

}  // namespace

Vector3 localOffset(const QGeoCoordinate& origin, const QGeoCoordinate& point)
{
    const double distance = origin.distanceTo(point);
    const double azimuth = qDegreesToRadians(origin.azimuthTo(point));
    return {distance * std::sin(azimuth), distance * std::cos(azimuth), point.altitude() - origin.altitude()};
}

double minimumSeparation(const Vector3& r0, const Vector3& delta)
{
    const double deltaSquared = dot(delta, delta);
    const double s = deltaSquared > 0 ? std::clamp(-dot(r0, delta) / deltaSquared, 0.0, 1.0) : 0.0;
    const Vector3 closest{r0.east + s * delta.east, r0.north + s * delta.north, r0.up + s * delta.up};
    return std::sqrt(dot(closest, closest));
}

QList<int> minimumCostAssignment(const QList<QList<double>>& cost)
{
    const int rows = cost.size();
    if (rows == 0)
        return {};
    const int columns = cost.first().size();
    if (columns < rows)
        return {};
    for (const QList<double>& row : cost) {
        if (row.size() != columns ||
            std::any_of(row.cbegin(), row.cend(), [](double entry) { return !std::isfinite(entry); }))
            return {};
    }
    // 势函数 + 增广路的匈牙利算法，O(rows²·columns)。下标从 1 起，第 0 列为虚拟列。
    constexpr double infinity = std::numeric_limits<double>::infinity();
    QList<double> rowPotential(rows + 1, 0.0);
    QList<double> columnPotential(columns + 1, 0.0);
    QList<int> columnRow(columns + 1, 0);  // 列当前匹配的行，0 表示未匹配
    QList<int> previousColumn(columns + 1, 0);
    for (int row = 1; row <= rows; ++row) {
        columnRow[0] = row;
        int column = 0;
        QList<double> slack(columns + 1, infinity);
        QList<bool> visited(columns + 1, false);
        do {
            visited[column] = true;
            const int currentRow = columnRow[column];
            double delta = infinity;
            int nextColumn = 0;
            for (int j = 1; j <= columns; ++j) {
                if (visited[j])
                    continue;
                const double reduced = cost[currentRow - 1][j - 1] - rowPotential[currentRow] - columnPotential[j];
                if (reduced < slack[j]) {
                    slack[j] = reduced;
                    previousColumn[j] = column;
                }
                if (slack[j] < delta) {
                    delta = slack[j];
                    nextColumn = j;
                }
            }
            // 数值溢出等异常时不再增广，避免死循环
            if (nextColumn == 0)
                return {};
            for (int j = 0; j <= columns; ++j) {
                if (visited[j]) {
                    rowPotential[columnRow[j]] += delta;
                    columnPotential[j] -= delta;
                } else {
                    slack[j] -= delta;
                }
            }
            column = nextColumn;
        } while (columnRow[column] != 0);
        do {
            const int previous = previousColumn[column];
            columnRow[column] = columnRow[previous];
            column = previous;
        } while (column != 0);
    }
    QList<int> assignment(rows, -1);
    for (int j = 1; j <= columns; ++j) {
        if (columnRow[j] != 0)
            assignment[columnRow[j] - 1] = j - 1;
    }
    return assignment;
}

double legHeading(const QGeoCoordinate& from, const QGeoCoordinate& to, double currentHeadingDeg, double spacing)
{
    if (!from.isValid() || !to.isValid() || from.distanceTo(to) < spacing)
        return normalizedHeading(currentHeadingDeg);
    return normalizedHeading(from.azimuthTo(to));
}

namespace {

constexpr double kSeparationTolerance = 0.05;  ///< 间距判据的数值容差（米）
constexpr double kStationaryTolerance = 0.01;  ///< 判定路径起点=终点（本航段静止）的容差（米）
constexpr double kReservedSlotCost = 1e12;     ///< 被占用槽位的分配代价：足够大，又不让算法溢出
constexpr double kPreferCurrentRatio = 1.05;   ///< 当前分配代价不超过最优的该倍数时保持当前分配

struct Pose
{
    QGeoCoordinate reference;
    double headingDeg = 0;
};

struct Path
{
    int id = 0;
    QGeoCoordinate start;
    QGeoCoordinate end;
};

Vector3 operator-(const Vector3& a, const Vector3& b)
{
    return {a.east - b.east, a.north - b.north, a.up - b.up};
}

double norm(const Vector3& v)
{
    return std::sqrt(dot(v, v));
}

/// 路径本航段是否静止：起点到终点的水平位移与高度差都小于容差（即起点=终点）。
bool isStationaryPath(const Vector3& start, const Vector3& end)
{
    const double horizontal = std::hypot(end.east - start.east, end.north - start.north);
    return horizontal < kStationaryTolerance && std::abs(end.up - start.up) < kStationaryTolerance;
}

double turnDegrees(double fromDeg, double toDeg)
{
    double turn = std::fmod(toDeg - fromDeg, 360.0);
    if (turn > 180.0)
        turn -= 360.0;
    else if (turn < -180.0)
        turn += 360.0;
    return turn;
}

Pose interpolatePose(const Pose& from, const Pose& to, double fraction)
{
    Pose pose;
    pose.reference = from.reference.atDistanceAndAzimuth(from.reference.distanceTo(to.reference) * fraction,
                                                         from.reference.azimuthTo(to.reference));
    pose.reference.setAltitude(from.reference.altitude() +
                               (to.reference.altitude() - from.reference.altitude()) * fraction);
    pose.headingDeg = normalizedHeading(from.headingDeg + turnDegrees(from.headingDeg, to.headingDeg) * fraction);
    return pose;
}

/// 设计 3.2 第 5 条：同步直线运动下任意两机的最小间距要求 required 由 m = min(起点间距, 终点间距) 决定：
/// m ≥ D_min + 余量时 required = D_min + 余量（不变）；D_min ≤ m < D_min + 余量时 required = D_min
/// （该对本就在跟踪余量之内起止，允许接近到硬下限但不得更近）；m < D_min 时 required = m（本就低于
/// 下限的机对不允许更近）。终点间距仍不得低于 D_min。不满足时写入 error。
bool separationAllowed(const QList<Path>& paths, double minSeparation, QString& error)
{
    if (paths.size() < 2)
        return true;
    const QGeoCoordinate origin = paths.first().start;
    QList<Vector3> starts;
    QList<Vector3> ends;
    for (const Path& path : paths) {
        starts.append(localOffset(origin, path.start));
        ends.append(localOffset(origin, path.end));
    }
    for (int i = 0; i < paths.size(); ++i) {
        for (int j = i + 1; j < paths.size(); ++j) {
            // 两条路径本航段都静止（起点=终点）：本次规划不移动它们，彼此间的既有距离与本次规划无关，
            // 不应据此拒绝整个计划（例如两个障碍物之间，或原地变队形时静止的长机与障碍物之间）。
            if (isStationaryPath(starts[i], ends[i]) && isStationaryPath(starts[j], ends[j]))
                continue;
            const Vector3 initial = starts[i] - starts[j];
            const Vector3 finish = ends[i] - ends[j];
            // 用 !(>=) 而非 (<) 写判据：任一侧为 NaN 时 (>=) 为 false，!(>=) 为 true，判定为不通过（失效关闭）。
            if (!(norm(finish) >= minSeparation - kSeparationTolerance)) {
                error = QCoreApplication::translate("FormationLegPlanner",
                                                    "%1 与 %2 号机终点间距 %3 米，低于安全下限 %4 米")
                            .arg(paths[i].id)
                            .arg(paths[j].id)
                            .arg(norm(finish), 0, 'f', 1)
                            .arg(minSeparation, 0, 'f', 1);
                return false;
            }
            const double closest = minimumSeparation(initial, finish - initial);
            const double tightest = std::min(norm(initial), norm(finish));
            const double required = tightest >= minSeparation + kTrackingMargin
                                        ? minSeparation + kTrackingMargin
                                        : (tightest >= minSeparation ? minSeparation : tightest);
            if (!(closest >= required - kSeparationTolerance)) {
                error =
                    QCoreApplication::translate("FormationLegPlanner", "%1 与 %2 号机路径最小间距 %3 米，低于 %4 米")
                        .arg(paths[i].id)
                        .arg(paths[j].id)
                        .arg(closest, 0, 'f', 1)
                        .arg(required, 0, 'f', 1);
                return false;
            }
        }
    }
    return true;
}

/// 位姿 pose 下全体成员（含长机）的终点
QHash<int, QGeoCoordinate> slotEnds(const Request& request, const QHash<int, int>& assignment, const Pose& pose)
{
    QHash<int, QGeoCoordinate> ends;
    ends.insert(request.leaderId, pose.reference);
    for (auto it = assignment.cbegin(); it != assignment.cend(); ++it)
        ends.insert(it.key(), slotPosition(pose.reference, pose.headingDeg, request.slotGeometries.at(it.value())));
    return ends;
}

Leg makeLeg(const Request& request, const QHash<int, QGeoCoordinate>& starts, const QHash<int, QGeoCoordinate>& ends,
            const Pose& pose)
{
    Leg leg;
    leg.reference = pose.reference;
    leg.headingDeg = pose.headingDeg;
    QList<int> ids = ends.keys();
    std::sort(ids.begin(), ids.end());
    double longest = 0;
    double vertical = 0;
    for (int id : std::as_const(ids)) {
        if (id == request.leaderId && !request.commandLeader)
            continue;
        LegMember entry;
        entry.id = id;
        entry.start = starts.value(id);
        entry.end = ends.value(id);
        entry.distance = entry.start.distanceTo(entry.end);
        longest = std::max(longest, entry.distance);
        vertical = std::max(vertical, std::abs(entry.end.altitude() - entry.start.altitude()));
        leg.members.append(entry);
    }
    const double horizontalSec = longest / request.speed;
    for (LegMember& entry : leg.members) {
        entry.speed = entry.distance >= kNegligibleDistance && horizontalSec > 0 ? entry.distance / horizontalSec : 0.0;
    }
    leg.durationSec = std::max(horizontalSec, vertical / kVerticalSpeedEstimate);
    return leg;
}

/// 参与间距校验的路径：受令成员 + 原地航段中静止的长机 + 不参与规划的空中成员（静止点）
QList<Path> legPaths(const Request& request, const Leg& leg, const QHash<int, QGeoCoordinate>& starts)
{
    QList<Path> paths;
    for (const LegMember& entry : leg.members)
        paths.append({entry.id, entry.start, entry.end});
    if (!request.commandLeader) {
        const QGeoCoordinate leader = starts.value(request.leaderId);
        paths.append({request.leaderId, leader, leader});
    }
    for (const Member& obstacle : request.obstacles)
        paths.append({obstacle.id, obstacle.position, obstacle.position});
    return paths;
}

/// 分层高度：长机（受令时）留在当前高度，僚机按当前高度由低到高逐层升高——保持抬升阶段的上下次序，
/// 避免水平靠近但当前高度顺序与槽位编号不一致的两机在抬升途中换位而短暂间距不足；高度相同则按槽位
/// 编号、再按 id 排序以保证结果确定。层距等于判据要求的间距。
QHash<int, double> layerAltitudes(const Request& request, const QHash<int, int>& assignment,
                                  const QHash<int, QGeoCoordinate>& positions)
{
    const double base = positions.value(request.leaderId).altitude();
    const double step = request.minSeparation + kTrackingMargin;
    QList<int> followers = assignment.keys();
    std::sort(followers.begin(), followers.end(), [&assignment, &positions](int a, int b) {
        const double altitudeA = positions.value(a).altitude();
        const double altitudeB = positions.value(b).altitude();
        if (altitudeA != altitudeB)
            return altitudeA < altitudeB;
        if (assignment.value(a) != assignment.value(b))
            return assignment.value(a) < assignment.value(b);
        return a < b;
    });
    QHash<int, double> layers;
    if (request.commandLeader)
        layers.insert(request.leaderId, base);
    for (int i = 0; i < followers.size(); ++i)
        layers.insert(followers[i], base + step * (i + 1));
    return layers;
}

/// from → to 的一段：依次尝试 1～kMaxSubLegs 均分，允许时再尝试垂直分层三段。
/// 成功时把航段追加到 legs，并把 positions 更新为最后一段的终点。
bool planSegment(const Request& request, const QHash<int, int>& assignment, const Pose& from, const Pose& to,
                 bool allowLayering, QHash<int, QGeoCoordinate>& positions, QList<Leg>& legs, QString& error)
{
    for (int parts = 1; parts <= kMaxSubLegs; ++parts) {
        QList<Leg> candidate;
        QHash<int, QGeoCoordinate> current = positions;
        bool separated = true;
        for (int part = 1; part <= parts && separated; ++part) {
            const Pose pose = interpolatePose(from, to, double(part) / parts);
            const Leg leg = makeLeg(request, current, slotEnds(request, assignment, pose), pose);
            separated = separationAllowed(legPaths(request, leg, current), request.minSeparation, error);
            for (const LegMember& entry : leg.members)
                current.insert(entry.id, entry.end);
            candidate.append(leg);
        }
        if (separated) {
            legs.append(candidate);
            positions = current;
            return true;
        }
    }
    if (!allowLayering)
        return false;
    // 垂直分层兜底：先只做垂直移动到各自高度层，再在层内水平移到终点正上方，最后回到槽位高度。
    const QHash<int, QGeoCoordinate> finalEnds = slotEnds(request, assignment, to);
    const QHash<int, double> layers = layerAltitudes(request, assignment, positions);
    QHash<int, QGeoCoordinate> lifted;
    QHash<int, QGeoCoordinate> crossed;
    for (auto it = layers.cbegin(); it != layers.cend(); ++it) {
        QGeoCoordinate up = positions.value(it.key());
        up.setAltitude(it.value());
        lifted.insert(it.key(), up);
        QGeoCoordinate across = finalEnds.value(it.key());
        across.setAltitude(it.value());
        crossed.insert(it.key(), across);
    }
    QList<Leg> layered;
    QHash<int, QGeoCoordinate> current = positions;
    const QList<std::pair<QHash<int, QGeoCoordinate>, Pose>> stages{{lifted, from}, {crossed, to}, {finalEnds, to}};
    for (const auto& [ends, pose] : stages) {
        const Leg leg = makeLeg(request, current, ends, pose);
        if (!separationAllowed(legPaths(request, leg, current), request.minSeparation, error))
            return false;
        for (const LegMember& entry : leg.members)
            current.insert(entry.id, entry.end);
        layered.append(leg);
    }
    legs.append(layered);
    positions = current;
    return true;
}

/// 僚机槽位分配：起点到目标位姿槽位的距离平方和最小；当前分配代价接近最优时优先保持当前分配。
/// 匈牙利算法失败（如代价矩阵异常）时返回空，不把失败结果当作槽位 0 静默接受。
std::optional<QHash<int, int>> assignSlots(const Request& request, const QList<int>& followers,
                                           const QHash<int, QGeoCoordinate>& positions, const Pose& target,
                                           bool preferCurrent)
{
    const int columns = request.slotGeometries.size();
    QList<QList<double>> cost(followers.size(), QList<double>(columns, kReservedSlotCost));
    for (int row = 0; row < followers.size(); ++row) {
        for (int column = 0; column < columns; ++column) {
            if (request.reservedSlots.contains(column))
                continue;
            const Vector3 offset =
                localOffset(positions.value(followers[row]),
                            slotPosition(target.reference, target.headingDeg, request.slotGeometries[column]));
            cost[row][column] = dot(offset, offset);
        }
    }
    const QList<int> optimal = minimumCostAssignment(cost);
    if (optimal.size() != followers.size())
        return std::nullopt;
    QHash<int, int> best;
    QHash<int, int> current;
    double bestCost = 0;
    double currentCost = 0;
    bool currentUsable = preferCurrent;
    QSet<int> used;
    for (int row = 0; row < followers.size(); ++row) {
        best.insert(followers[row], optimal.value(row));
        bestCost += cost[row][optimal.value(row)];
        const int slot = request.currentAssignment.value(followers[row], -1);
        if (slot < 0 || slot >= columns || request.reservedSlots.contains(slot) || used.contains(slot)) {
            currentUsable = false;
            continue;
        }
        used.insert(slot);
        current.insert(followers[row], slot);
        currentCost += cost[row][slot];
    }
    return currentUsable && currentCost <= bestCost * kPreferCurrentRatio + 1e-6 ? current : best;
}

}  // namespace

QGeoCoordinate slotPosition(const QGeoCoordinate& reference, double headingDeg, const SlotGeometry& slot)
{
    QGeoCoordinate position = reference.atDistanceAndAzimuth(slot.distance, headingDeg + slot.bearing);
    position.setAltitude(reference.altitude() + slot.altitude);
    return position;
}

double Plan::durationSec() const
{
    double total = 0;
    for (const Leg& leg : legs)
        total += leg.durationSec;
    return total;
}

Plan plan(const Request& request)
{
    Plan result;
    const auto fail = [&result](const QString& error) {
        result.legs.clear();
        result.assignment.clear();
        result.error = error;
        return result;
    };
    QHash<int, QGeoCoordinate> positions;
    for (const Member& entry : request.members) {
        if (!entry.position.isValid() || !std::isfinite(entry.position.altitude()))
            return fail(QCoreApplication::translate("FormationLegPlanner", "%1 号机位置或高度无效").arg(entry.id));
        positions.insert(entry.id, entry.position);
    }
    for (const Member& obstacle : request.obstacles) {
        if (!obstacle.position.isValid() || !std::isfinite(obstacle.position.altitude()))
            return fail(QCoreApplication::translate("FormationLegPlanner", "%1 号机位置或高度无效").arg(obstacle.id));
    }
    if (!positions.contains(request.leaderId))
        return fail(QCoreApplication::translate("FormationLegPlanner", "长机 %1 不在编队成员中").arg(request.leaderId));
    if (request.waypoints.isEmpty() || !(request.speed > 0) || !std::isfinite(request.speed) ||
        !(request.minSeparation > 0) || !std::isfinite(request.minSeparation) || !std::isfinite(request.spacing) ||
        !std::isfinite(request.startHeadingDeg) || !request.startReference.isValid() ||
        !std::isfinite(request.startReference.altitude()))
        return fail(QCoreApplication::translate("FormationLegPlanner", "航段参数无效"));
    for (const SlotGeometry& slot : request.slotGeometries) {
        if (!std::isfinite(slot.distance) || !std::isfinite(slot.bearing) || !std::isfinite(slot.altitude))
            return fail(QCoreApplication::translate("FormationLegPlanner", "航段参数无效"));
    }
    for (const QGeoCoordinate& waypoint : request.waypoints) {
        if (!waypoint.isValid() || !std::isfinite(waypoint.altitude()))
            return fail(QCoreApplication::translate("FormationLegPlanner", "航点坐标或高度无效"));
    }
    QList<int> followers = positions.keys();
    followers.removeAll(request.leaderId);
    std::sort(followers.begin(), followers.end());
    if (followers.size() + request.reservedSlots.size() > request.slotGeometries.size())
        return fail(QCoreApplication::translate("FormationLegPlanner", "编队槽位数量不足"));

    const Pose start{request.startReference, normalizedHeading(request.startHeadingDeg)};
    QList<Pose> poses;
    Pose previous = start;
    for (const QGeoCoordinate& waypoint : request.waypoints) {
        const Pose pose{waypoint, legHeading(previous.reference, waypoint, previous.headingDeg, request.spacing)};
        poses.append(pose);
        previous = pose;
    }

    // 尝试顺序（设计 3.2 第 6 条）：当前分配不分层 → 最优分配不分层 → 最优分配 + 垂直分层
    struct Attempt
    {
        bool preferCurrent;
        bool allowLayering;
    };

    QString error;
    for (const Attempt attempt : {Attempt{true, false}, Attempt{false, false}, Attempt{false, true}}) {
        const std::optional<QHash<int, int>> assigned =
            assignSlots(request, followers, positions, poses.first(), attempt.preferCurrent);
        if (!assigned) {
            error = QCoreApplication::translate("FormationLegPlanner", "槽位分配失败");
            continue;
        }
        const QHash<int, int>& assignment = *assigned;
        QList<Leg> legs;
        QHash<int, QGeoCoordinate> current = positions;
        Pose from = start;
        bool planned = true;
        for (const Pose& pose : std::as_const(poses)) {
            if (!planSegment(request, assignment, from, pose, attempt.allowLayering, current, legs, error)) {
                planned = false;
                break;
            }
            from = pose;
        }
        if (!planned)
            continue;
        // 路径越界不立即判失败：换个分配/分层方式可能绕开限制区域，记下原因等全部尝试失败再报告。
        bool pathRejected = false;
        for (const Leg& leg : std::as_const(legs)) {
            for (const LegMember& entry : leg.members) {
                const QString rejection =
                    request.pathCheck ? request.pathCheck(entry.id, entry.start, entry.end) : QString();
                if (!rejection.isEmpty()) {
                    error = QCoreApplication::translate("FormationLegPlanner", "%1 号机航段被拒绝：%2")
                                .arg(QString::number(entry.id), rejection);
                    pathRejected = true;
                    break;
                }
            }
            if (pathRejected)
                break;
        }
        if (pathRejected)
            continue;
        result.legs = legs;
        result.assignment = assignment;
        return result;
    }
    return fail(error);
}

}  // namespace FormationLegPlanner
