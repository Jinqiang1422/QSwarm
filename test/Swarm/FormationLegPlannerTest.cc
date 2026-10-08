#include "FormationLegPlannerTest.h"

#include <QtCore/QScopeGuard>
#include <QtCore/QtMath>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>

#include "FormationLegPlanner.h"
#include "FormationLegTracker.h"
#include "MissionItem.h"
#include "SwarmMissionSnapshot.h"

using namespace FormationLegPlanner;

namespace {

const QGeoCoordinate kOrigin(47.397, 8.5455, 500);

/// 密集采样求最小间距，用于独立复核闭式解
double sampledSeparation(const Vector3& r0, const Vector3& delta)
{
    double best = std::numeric_limits<double>::max();
    for (int i = 0; i <= 10000; ++i) {
        const double s = i / 10000.0;
        best = std::min(best, std::hypot(r0.east + s * delta.east, r0.north + s * delta.north, r0.up + s * delta.up));
    }
    return best;
}

/// 暴力枚举所有行→列排列，求最小总代价，用于独立复核匈牙利解
double bruteForceAssignmentCost(const QList<QList<double>>& cost)
{
    const int rows = cost.size();
    const int columns = cost.first().size();
    QList<int> order(columns);
    std::iota(order.begin(), order.end(), 0);
    double best = std::numeric_limits<double>::max();
    do {
        double sum = 0;
        for (int r = 0; r < rows; ++r)
            sum += cost[r][order[r]];
        best = std::min(best, sum);
    } while (std::next_permutation(order.begin(), order.end()));
    return best;
}

/// 原点以东 east 米、以北 north 米、高 up 米处
QGeoCoordinate at(double east, double north, double up = 0)
{
    QGeoCoordinate point =
        kOrigin.atDistanceAndAzimuth(std::hypot(east, north), qRadiansToDegrees(std::atan2(east, north)));
    point.setAltitude(kOrigin.altitude() + up);
    return point;
}

Vector3 sub(const Vector3& a, const Vector3& b)
{
    return {a.east - b.east, a.north - b.north, a.up - b.up};
}

double length(const Vector3& v)
{
    return std::hypot(v.east, v.north, v.up);
}

/// 与 FormationController::_formationOffset 楔形一致的槽位
QList<SlotGeometry> wedgeSlots(int count, double spacing, double verticalStep)
{
    QList<SlotGeometry> slotGeometries;
    for (int index = 1; index <= count; ++index) {
        const int rank = (index + 1) / 2;
        const int side = index % 2 == 1 ? -1 : 1;
        slotGeometries.append({spacing * rank, 180.0 + side * 30.0, verticalStep * rank});
    }
    return slotGeometries;
}

/// 按设计 3.2 第 5 条独立复核每段的成对间距（密集采样，不依赖被测闭式解）。stationary 为不受令的静止点。
void verifyLegSeparation(const Plan& result, double minSeparation, const QList<QGeoCoordinate>& stationary = {})
{
    for (const Leg& leg : result.legs) {
        QList<std::pair<QGeoCoordinate, QGeoCoordinate>> paths;
        for (const LegMember& member : leg.members)
            paths.append({member.start, member.end});
        for (const QGeoCoordinate& point : stationary)
            paths.append({point, point});
        for (int i = 0; i < paths.size(); ++i) {
            for (int j = i + 1; j < paths.size(); ++j) {
                const Vector3 initial = sub(localOffset(kOrigin, paths[i].first), localOffset(kOrigin, paths[j].first));
                const Vector3 finish =
                    sub(localOffset(kOrigin, paths[i].second), localOffset(kOrigin, paths[j].second));
                const double closest = sampledSeparation(initial, sub(finish, initial));
                const double tightest = std::min(length(initial), length(finish));
                const double required = tightest >= minSeparation + kTrackingMargin
                                            ? minSeparation + kTrackingMargin
                                            : (tightest >= minSeparation ? minSeparation : tightest);
                QVERIFY2(closest >= required - 0.1, qPrintable(QStringLiteral("paths %1/%2 come within %3 m, need %4 m")
                                                                   .arg(i)
                                                                   .arg(j)
                                                                   .arg(closest)
                                                                   .arg(required)));
                QVERIFY2(length(finish) >= minSeparation - 0.1,
                         qPrintable(QStringLiteral("paths %1/%2 end %3 m apart, need %4 m")
                                        .arg(i)
                                        .arg(j)
                                        .arg(length(finish))
                                        .arg(minSeparation)));
            }
        }
    }
}

/// 每段成员起点衔接上一段终点；首段起点等于请求里各成员的初始位置。
void verifyLegContinuity(const Request& request, const Plan& result)
{
    QHash<int, QGeoCoordinate> previousEnd;
    for (const Member& entry : request.members)
        previousEnd.insert(entry.id, entry.position);
    for (const Leg& leg : result.legs) {
        for (const LegMember& entry : leg.members) {
            QVERIFY(previousEnd.contains(entry.id));
            const QGeoCoordinate expectedStart = previousEnd.value(entry.id);
            QVERIFY2(entry.start.distanceTo(expectedStart) < 0.01,
                     qPrintable(QStringLiteral("member %1 start drifted from previous leg's end").arg(entry.id)));
            QVERIFY(qAbs(entry.start.altitude() - expectedStart.altitude()) < 0.01);
            previousEnd.insert(entry.id, entry.end);
        }
    }
}

const LegMember* member(const Leg& leg, int id)
{
    const auto it = std::find_if(leg.members.cbegin(), leg.members.cend(),
                                 [id](const LegMember& candidate) { return candidate.id == id; });
    return it == leg.members.cend() ? nullptr : &*it;
}

/// 长机 1 位于原点、只有一架僚机 2 的纵队（无高度分层）
Request columnPair(const QGeoCoordinate& follower, double startHeading, const QGeoCoordinate& waypoint)
{
    Request request;
    request.leaderId = 1;
    request.members = {{1, at(0, 0)}, {2, follower}};
    request.slotGeometries = {{10, 180, 0}};
    request.currentAssignment = {{2, 0}};
    request.startReference = at(0, 0);
    request.startHeadingDeg = startHeading;
    request.waypoints = {waypoint};
    request.spacing = 10;
    return request;
}

/// 两机并排向东飞 100 m（相距 10 m），基准速度 5 m/s，时长 20 s
Leg parallelLeg()
{
    Leg leg;
    leg.durationSec = 20;
    leg.members = {{1, at(0, 0), at(100, 0), 100, 5}, {2, at(0, 10), at(100, 10), 100, 5}};
    return leg;
}

}  // namespace

void FormationLegPlannerTest::_localOffsetMatchesBearing()
{
    const Vector3 east = localOffset(kOrigin, kOrigin.atDistanceAndAzimuth(100, 90, 10));
    QVERIFY(qAbs(east.east - 100.0) < 0.01);
    QVERIFY(qAbs(east.north) < 0.01);
    QVERIFY(qAbs(east.up - 10.0) < 0.01);

    const Vector3 north = localOffset(kOrigin, kOrigin.atDistanceAndAzimuth(100, 0, 10));
    QVERIFY(qAbs(north.east) < 0.01);
    QVERIFY(qAbs(north.north - 100.0) < 0.01);
    QVERIFY(qAbs(north.up - 10.0) < 0.01);

    const Vector3 southwest = localOffset(kOrigin, kOrigin.atDistanceAndAzimuth(100, 225, 10));
    QVERIFY(qAbs(southwest.east - (-70.71)) < 0.01);
    QVERIFY(qAbs(southwest.north - (-70.71)) < 0.01);
    QVERIFY(qAbs(southwest.up - 10.0) < 0.01);

    const Vector3 same = localOffset(kOrigin, kOrigin);
    QVERIFY(qAbs(same.east) < 0.01);
    QVERIFY(qAbs(same.north) < 0.01);
    QVERIFY(qAbs(same.up) < 0.01);
}

void FormationLegPlannerTest::_closedFormSeparationMatchesSampling()
{
    std::mt19937 random(20261005);
    std::uniform_real_distribution<double> value(-50.0, 50.0);
    for (int i = 0; i < 200; ++i) {
        const Vector3 r0{value(random), value(random), value(random)};
        const Vector3 delta{value(random), value(random), value(random)};
        const double closedForm = minimumSeparation(r0, delta);
        const double sampled = sampledSeparation(r0, delta);
        QVERIFY2(qAbs(closedForm - sampled) < 0.01,
                 qPrintable(QString("iteration %1: closedForm=%2 sampled=%3").arg(i).arg(closedForm).arg(sampled)));
    }
    // A head-on swap passes through zero; a pure translation keeps its separation.
    QVERIFY(qAbs(minimumSeparation({0, 10, 0}, {0, -20, 0})) < 1e-9);
    QVERIFY(qAbs(minimumSeparation({3, 4, 0}, {0, 0, 0}) - 5.0) < 1e-9);
}

void FormationLegPlannerTest::_assignmentMatchesBruteForce()
{
    std::mt19937 random(7);
    std::uniform_real_distribution<double> value(0.0, 100.0);
    for (int rows = 1; rows <= 5; ++rows) {
        for (int columns = rows; columns <= 6; ++columns) {
            QList<QList<double>> cost(rows, QList<double>(columns));
            for (QList<double>& row : cost) {
                for (double& entry : row)
                    entry = value(random);
            }
            const QList<int> assignment = minimumCostAssignment(cost);
            QCOMPARE(assignment.size(), rows);
            QCOMPARE(QSet<int>(assignment.cbegin(), assignment.cend()).size(), rows);
            double total = 0;
            for (int r = 0; r < rows; ++r)
                total += cost[r][assignment[r]];
            const double best = bruteForceAssignmentCost(cost);
            QVERIFY2(qAbs(total - best) < 1e-9,
                     qPrintable(
                         QString("rows=%1 columns=%2: total=%3 best=%4").arg(rows).arg(columns).arg(total).arg(best)));
        }
    }
    QVERIFY(minimumCostAssignment({{1.0, 2.0}, {3.0, 4.0}, {5.0, 6.0}}).isEmpty());

    // 大量同代价的退化矩阵（4x4，取值 0..2）仍应与暴力解一致。
    std::mt19937 tiedRandom(42);
    std::uniform_int_distribution<int> tiedValue(0, 2);
    for (int iteration = 0; iteration < 20; ++iteration) {
        QList<QList<double>> tiedCost(4, QList<double>(4));
        for (QList<double>& row : tiedCost) {
            for (double& entry : row)
                entry = tiedValue(tiedRandom);
        }
        const QList<int> assignment = minimumCostAssignment(tiedCost);
        QCOMPARE(assignment.size(), 4);
        QCOMPARE(QSet<int>(assignment.cbegin(), assignment.cend()).size(), 4);
        double total = 0;
        for (int r = 0; r < 4; ++r)
            total += tiedCost[r][assignment[r]];
        const double best = bruteForceAssignmentCost(tiedCost);
        QVERIFY2(qAbs(total - best) < 1e-9,
                 qPrintable(QString("tied iteration=%1: total=%2 best=%3").arg(iteration).arg(total).arg(best)));
    }
}

void FormationLegPlannerTest::_assignmentRejectsInvalidInput()
{
    QVERIFY(minimumCostAssignment({{1.0, qQNaN()}, {2.0, 3.0}}).isEmpty());
    QVERIFY(minimumCostAssignment({{qInf(), 1.0}}).isEmpty());
    QVERIFY(minimumCostAssignment({{1.0, 2.0}, {3.0}}).isEmpty());
    QVERIFY(minimumCostAssignment({}).isEmpty());
}

void FormationLegPlannerTest::_legHeadingKeepsOrientationForShortMoves()
{
    const double north = legHeading(kOrigin, kOrigin.atDistanceAndAzimuth(100, 0), 90, 15);
    QVERIFY(north < 0.01 || north > 359.99);
    QVERIFY(qAbs(legHeading(kOrigin, kOrigin.atDistanceAndAzimuth(100, 225), 90, 15) - 225) < 0.01);
    QCOMPARE(legHeading(kOrigin, kOrigin.atDistanceAndAzimuth(10, 180), 90, 15), 90.0);
    QCOMPARE(legHeading(kOrigin, kOrigin.atDistanceAndAzimuth(10, 180), -90, 15), 270.0);

    // normalizedHeading 不应因浮点舍入返回 360（而不是归一化区间内的 0）。
    const double nearZero = legHeading(kOrigin, kOrigin, -1e-15, 15);
    QVERIFY(nearZero < 360.0);
    QCOMPARE(nearZero, 0.0);

    // 短距离移动时保持当前朝向，但仍按 [0, 360) 归一化，即使入参超出该范围。
    QCOMPARE(legHeading(kOrigin, kOrigin.atDistanceAndAzimuth(10, 180), 450, 15), 90.0);

    // from 坐标无效时保持（归一化的）当前朝向。
    QCOMPARE(legHeading(QGeoCoordinate(), kOrigin, 90, 15), 90.0);
}

void FormationLegPlannerTest::_pureTranslationKeepsShape()
{
    const QList<SlotGeometry> wedge = wedgeSlots(4, 15, 3);
    Request request;
    request.leaderId = 1;
    request.members.append({1, at(0, 0)});
    for (int k = 0; k < 4; ++k)
        request.members.append({k + 2, slotPosition(at(0, 0), 0, wedge[k])});
    request.slotGeometries = wedge;
    request.currentAssignment = {{2, 0}, {3, 1}, {4, 2}, {5, 3}};
    request.startReference = at(0, 0);
    request.startHeadingDeg = 0;
    request.waypoints = {at(0, 150)};
    request.spacing = 15;
    const Plan result = plan(request);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.legs.size(), 1);
    QVERIFY(result.assignment == request.currentAssignment);
    const Leg& leg = result.legs.first();
    QVERIFY(qAbs(leg.durationSec - 30.0) < 0.01);
    QCOMPARE(leg.members.size(), 5);
    for (const LegMember& entry : leg.members) {
        QVERIFY(qAbs(entry.speed - 5.0) < 0.01);
        const QGeoCoordinate expected = entry.id == 1 ? at(0, 150) : slotPosition(at(0, 150), 0, wedge[entry.id - 2]);
        QVERIFY(entry.end.distanceTo(expected) < 0.01);
        QVERIFY(qAbs(entry.end.altitude() - expected.altitude()) < 0.01);
    }
    verifyLegSeparation(result, request.minSeparation);
}

void FormationLegPlannerTest::_routePlansOneLegPerWaypoint()
{
    const QList<SlotGeometry> wedge = wedgeSlots(2, 10, 3);
    Request request;
    request.leaderId = 1;
    request.members = {
        {1, at(0, 0)}, {2, slotPosition(at(0, 0), 90, wedge[0])}, {3, slotPosition(at(0, 0), 90, wedge[1])}};
    request.slotGeometries = wedge;
    request.currentAssignment = {{2, 0}, {3, 1}};
    request.startReference = at(0, 0);
    request.startHeadingDeg = 90;
    request.waypoints = {at(100, 0), at(100, 100, 5)};
    request.spacing = 10;
    const Plan result = plan(request);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.legs.size(), 2);
    QVERIFY(qAbs(result.legs[0].headingDeg - 90) < 0.01);
    const double turned = result.legs[1].headingDeg;
    QVERIFY(turned < 0.01 || turned > 359.99);
    const Leg& last = result.legs[1];
    const LegMember* leader = member(last, 1);
    QVERIFY(leader);
    QVERIFY(leader->end.distanceTo(at(100, 100)) < 0.01);
    QVERIFY(qAbs(leader->end.altitude() - 505) < 0.01);
    for (int id : {2, 3}) {
        const QGeoCoordinate expected = slotPosition(at(100, 100, 5), turned, wedge[id - 2]);
        const LegMember* follower = member(last, id);
        QVERIFY(follower);
        QVERIFY(follower->end.distanceTo(expected) < 0.01);
        QVERIFY(qAbs(follower->end.altitude() - 508) < 0.01);
    }
    QVERIFY(qAbs(result.durationSec() - 40.0) < 0.1);
    // 每机指令地速按水平航程比例分配：speed * horizontalTime == distance（horizontalTime 取本段最长水平航程/编队速度，
    // 不用 leg.durationSec——它可能取的是垂直估算时长）。
    for (const Leg& leg : result.legs) {
        double longest = 0;
        for (const LegMember& entry : leg.members)
            longest = std::max(longest, entry.distance);
        const double horizontalTime = longest / request.speed;
        for (const LegMember& entry : leg.members) {
            if (entry.distance >= kNegligibleDistance)
                QVERIFY(qAbs(entry.speed * horizontalTime - entry.distance) < 0.01);
        }
    }
    verifyLegContinuity(request, result);
    verifyLegSeparation(result, request.minSeparation);
}

void FormationLegPlannerTest::_largeTurnSplitsIntoSubLegs()
{
    // Reversing over 12 m would swap leader and follower head-on; two 90° halves keep them apart.
    // (12 m rather than exactly one spacing, so the course-change threshold is not a floating-point tie.)
    const Request request = columnPair(at(0, -10), 0, at(0, -12));
    const Plan result = plan(request);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.legs.size(), 2);
    const LegMember* leaderLeg0 = member(result.legs[0], 1);
    QVERIFY(leaderLeg0);
    QVERIFY(leaderLeg0->end.distanceTo(at(0, -6)) < 0.01);
    const LegMember* followerLeg0 = member(result.legs[0], 2);
    QVERIFY(followerLeg0);
    // 90° 转弧可能向左也可能向右绕出，两个方向都保持间距，不固定具体一侧。
    QVERIFY(followerLeg0->end.distanceTo(at(-10, -6)) < 0.01 || followerLeg0->end.distanceTo(at(10, -6)) < 0.01);
    const LegMember* leaderLeg1 = member(result.legs[1], 1);
    QVERIFY(leaderLeg1);
    QVERIFY(leaderLeg1->end.distanceTo(at(0, -12)) < 0.01);
    const LegMember* followerLeg1 = member(result.legs[1], 2);
    QVERIFY(followerLeg1);
    QVERIFY(followerLeg1->end.distanceTo(at(0, -2)) < 0.01);
    verifyLegContinuity(request, result);
    verifyLegSeparation(result, 3);
}

void FormationLegPlannerTest::_layersWhenNoSplitSeparates()
{
    // With a 9.5 m minimum even quarter turns bring the pair too close; the follower crosses on its own layer.
    Request request = columnPair(at(0, -10), 0, at(0, -12));
    request.minSeparation = 9.5;
    const Plan result = plan(request);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.legs.size(), 3);
    const LegMember* lift = member(result.legs[0], 2);
    QVERIFY(lift);
    QVERIFY(lift->distance < 0.01);
    QCOMPARE(lift->speed, 0.0);  // 纯垂直移动，水平航程可忽略，不下发水平速度
    QVERIFY(qAbs(lift->end.altitude() - 510.5) < 0.01);
    const LegMember* cross = member(result.legs[1], 2);
    QVERIFY(cross);
    QVERIFY(cross->end.distanceTo(at(0, -2)) < 0.01);
    QVERIFY(qAbs(cross->end.altitude() - 510.5) < 0.01);
    const LegMember* leaderLeg1 = member(result.legs[1], 1);
    QVERIFY(leaderLeg1);
    QVERIFY(qAbs(leaderLeg1->end.altitude() - 500) < 0.01);
    const LegMember* followerLeg2 = member(result.legs[2], 2);
    QVERIFY(followerLeg2);
    QVERIFY(qAbs(followerLeg2->end.altitude() - 500) < 0.01);
    verifyLegContinuity(request, result);
    verifyLegSeparation(result, 9.5);
}

void FormationLegPlannerTest::_rejectsEndSlotsCloserThanMinimum()
{
    Request request = columnPair(at(0, -10), 0, at(0, -12));
    request.minSeparation = 12;
    const Plan result = plan(request);
    QVERIFY(!result.ok());
    QVERIFY2(result.error.contains(QStringLiteral("终点间距")), qPrintable(result.error));
}

void FormationLegPlannerTest::_closePairMayOnlyDiverge()
{
    // SITL spawns aircraft 2.5 m apart. Moving away is fine; crossing the hovering leader goes over it.
    Request diverging = columnPair(at(-2.5, 0), 90, at(0, 0));
    diverging.commandLeader = false;
    const Plan away = plan(diverging);
    QVERIFY2(away.ok(), qPrintable(away.error));
    QCOMPARE(away.legs.size(), 1);
    QCOMPARE(away.legs.first().members.size(), 1);
    verifyLegContinuity(diverging, away);
    verifyLegSeparation(away, 3, {at(0, 0)});

    Request crossing = columnPair(at(2.5, 0), 90, at(0, 0));
    crossing.commandLeader = false;
    const Plan over = plan(crossing);
    QVERIFY2(over.ok(), qPrintable(over.error));
    QCOMPARE(over.legs.size(), 3);
    for (const Leg& leg : over.legs)
        QVERIFY(!member(leg, 1));
    const LegMember* followerLeg1 = member(over.legs[1], 2);
    QVERIFY(followerLeg1);
    QVERIFY(qAbs(followerLeg1->end.altitude() - 504) < 0.01);
    const LegMember* followerLeg2 = member(over.legs[2], 2);
    QVERIFY(followerLeg2);
    QVERIFY(followerLeg2->end.distanceTo(at(-10, 0)) < 0.01);
    verifyLegContinuity(crossing, over);
    verifyLegSeparation(over, 3, {at(0, 0)});
}

void FormationLegPlannerTest::_spawnLineReshapeIsPlanned()
{
    // 2026-10-08 SITL 回归：5 机在一条线上出生，间距 3 m，高度带抖动。原地变队形（wedge）时，若垂直分层按槽位
    // 编号排序，抬升阶段会让水平相距仅 3 m 的两机互换上下次序，导致间距短暂跌到 3.0 m，低于判据要求的 3.1 m
    // 而被拒绝。layerAltitudes 改按当前高度排序、separationAllowed 放宽判据后，该队形必须能规划成功。
    const QList<SlotGeometry> wedge = wedgeSlots(4, 15, 3);
    Request request;
    request.leaderId = 1;
    request.members = {
        {1, at(0, 0, 0)}, {2, at(3, 0, 0.6)}, {3, at(6, 0, 0.2)}, {4, at(9, 0, 1.2)}, {5, at(12, 0, 0.3)}};
    request.slotGeometries = wedge;
    request.currentAssignment = {{2, 0}, {3, 1}, {4, 2}, {5, 3}};
    request.startReference = at(0, 0, 0);
    request.startHeadingDeg = 89;
    request.waypoints = {at(0, 0, 0)};
    request.commandLeader = false;
    request.spacing = 15;
    request.minSeparation = 3;
    const Plan result = plan(request);
    QVERIFY2(result.ok(), qPrintable(result.error));
    verifyLegSeparation(result, 3, {at(0, 0, 0)});
    verifyLegContinuity(request, result);

    QHash<int, QGeoCoordinate> finalEnd;
    for (const Member& entry : request.members)
        finalEnd.insert(entry.id, entry.position);
    for (const Leg& leg : result.legs) {
        for (const LegMember& entry : leg.members)
            finalEnd.insert(entry.id, entry.end);
    }
    for (int id : {2, 3, 4, 5}) {
        QVERIFY(result.assignment.contains(id));
        const QGeoCoordinate expected = slotPosition(at(0, 0, 0), 89, wedge[result.assignment.value(id)]);
        QVERIFY2(finalEnd.value(id).distanceTo(expected) < 0.01,
                 qPrintable(QStringLiteral("member %1 did not reach its slot").arg(id)));
        QVERIFY(qAbs(finalEnd.value(id).altitude() - expected.altitude()) < 0.01);
    }
}

void FormationLegPlannerTest::_keepsCurrentAssignmentWhenNearlyOptimal()
{
    // Followers far to the south and north reach either slot of the line for the same cost.
    for (const QHash<int, int>& current : {QHash<int, int>{{2, 1}, {3, 0}}, QHash<int, int>{{2, 0}, {3, 1}}}) {
        Request request;
        request.leaderId = 1;
        request.members = {{1, at(0, 0)}, {2, at(0, -1000)}, {3, at(0, 1000)}};
        request.slotGeometries = {{10, 270, 0}, {10, 90, 0}};
        request.currentAssignment = current;
        request.startReference = at(0, 0);
        request.startHeadingDeg = 0;
        request.waypoints = {at(0, 0)};
        request.commandLeader = false;
        const Plan result = plan(request);
        QVERIFY2(result.ok(), qPrintable(result.error));
        QVERIFY(result.assignment == current);
    }
}

void FormationLegPlannerTest::_crossingCurrentAssignmentFallsBackToOptimal()
{
    Request request;
    request.leaderId = 1;
    request.members = {{1, at(0, 0)}, {2, at(-10, -1000)}, {3, at(10, -1000)}};
    request.slotGeometries = {{10, 270, 0}, {10, 90, 0}};
    request.currentAssignment = {{2, 1}, {3, 0}};  // nearly optimal in cost, but the paths cross
    request.startReference = at(0, 0);
    request.startHeadingDeg = 0;
    request.waypoints = {at(0, 0)};
    request.commandLeader = false;
    const Plan result = plan(request);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QVERIFY((result.assignment == QHash<int, int>{{2, 0}, {3, 1}}));
    QCOMPARE(result.legs.size(), 1);
}

void FormationLegPlannerTest::_reservedSlotsStayWithTheirHolders()
{
    const QList<SlotGeometry> wedge = wedgeSlots(2, 10, 3);
    Request request;
    request.leaderId = 1;
    request.members = {{1, at(0, 0)}, {2, slotPosition(at(0, 0), 0, wedge[0])}};
    request.slotGeometries = wedge;
    request.reservedSlots = {0};  // held by a member that is still on the ground
    request.currentAssignment = {{2, 1}, {4, 0}};
    request.startReference = at(0, 0);
    request.startHeadingDeg = 0;
    request.waypoints = {at(0, 0)};
    request.commandLeader = false;
    const Plan result = plan(request);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QVERIFY((result.assignment == QHash<int, int>{{2, 1}}));
}

void FormationLegPlannerTest::_pathOutsideAreaRejected()
{
    Request request = columnPair(at(0, -10), 0, at(0, 100));
    request.pathCheck = [](int id, const QGeoCoordinate&, const QGeoCoordinate&) {
        return id == 2 ? QStringLiteral("越出电子围栏") : QString();
    };
    const Plan result = plan(request);
    QVERIFY(!result.ok());
    QVERIFY2(result.error.contains(QStringLiteral("2 号机航段被拒绝：越出电子围栏")), qPrintable(result.error));
}

void FormationLegPlannerTest::_airborneObstacleKeepsPathsClear()
{
    // Leader 1 with follower 2 abreast 10 m to its east, flying 100 m north: without obstacles the formation
    // translates in a single leg.
    Request request;
    request.leaderId = 1;
    request.members = {{1, at(0, 0)}, {2, at(10, 0)}};
    request.slotGeometries = {{10, 90, 0}};
    request.currentAssignment = {{2, 0}};
    request.startReference = at(0, 0);
    request.startHeadingDeg = 0;
    request.waypoints = {at(0, 100)};
    request.spacing = 10;
    const Plan straight = plan(request);
    QVERIFY2(straight.ok(), qPrintable(straight.error));
    QCOMPARE(straight.legs.size(), 1);

    // An airborne member outside the plan hovers 1 m beside the follower's straight path. The follower must not
    // pass within D_min + margin of it: it climbs onto its own layer, crosses there and descends.
    const QGeoCoordinate besideFollower = at(11, 50);
    request.obstacles = {{3, besideFollower}};
    const Plan layered = plan(request);
    QVERIFY2(layered.ok(), qPrintable(layered.error));
    QCOMPARE(layered.legs.size(), 3);
    for (const Leg& leg : layered.legs)
        QVERIFY(!member(leg, 3));
    verifyLegContinuity(request, layered);
    verifyLegSeparation(layered, request.minSeparation, {besideFollower});

    // Beside the leader's path no layer helps (the leader keeps its altitude), so the goto is refused.
    request.obstacles = {{3, at(1, 50)}};
    const Plan refused = plan(request);
    QVERIFY(!refused.ok());
    QVERIFY2(refused.error.contains(QStringLiteral("1 与 3 号机")), qPrintable(refused.error));
}

void FormationLegPlannerTest::_stationaryPairsDoNotBlockPlan()
{
    // Two obstacles 1 m apart (well inside D_min) but far (> 50 m) from every moving path: this leg moves
    // neither of them, so their mutual distance must not refuse the plan.
    Request goto_;
    goto_.leaderId = 1;
    goto_.members = {{1, at(0, 0)}, {2, at(10, 0)}};
    goto_.slotGeometries = {{10, 90, 0}};
    goto_.currentAssignment = {{2, 0}};
    goto_.startReference = at(0, 0);
    goto_.startHeadingDeg = 0;
    goto_.waypoints = {at(0, 100)};
    goto_.spacing = 10;
    goto_.obstacles = {{3, at(500, 500)}, {4, at(501, 500)}};
    const Plan gotoResult = plan(goto_);
    QVERIFY2(gotoResult.ok(), qPrintable(gotoResult.error));
    QCOMPARE(gotoResult.legs.size(), 1);

    // In-place reshape: the hovering leader and an obstacle 1 m away are both stationary (the leg only moves
    // the follower, far off to the east), so their close distance must not refuse the plan either.
    Request reshape;
    reshape.leaderId = 1;
    reshape.members = {{1, at(0, 0)}, {2, at(500, 0)}};
    reshape.slotGeometries = {{10, 90, 0}};
    reshape.currentAssignment = {{2, 0}};
    reshape.startReference = at(0, 0);
    reshape.startHeadingDeg = 0;
    reshape.waypoints = {at(0, 0)};
    reshape.spacing = 10;
    reshape.commandLeader = false;
    reshape.obstacles = {{3, at(1, 0)}};
    const Plan reshapeResult = plan(reshape);
    QVERIFY2(reshapeResult.ok(), qPrintable(reshapeResult.error));
}

void FormationLegPlannerTest::_invalidInputsFailClosed()
{
    Request badHeading = columnPair(at(0, -10), 0, at(0, -12));
    badHeading.startHeadingDeg = qQNaN();
    QVERIFY(!plan(badHeading).ok());

    Request badSlot = columnPair(at(0, -10), 0, at(0, -12));
    badSlot.slotGeometries[0].altitude = qQNaN();
    QVERIFY(!plan(badSlot).ok());

    Request badSpeed = columnPair(at(0, -10), 0, at(0, -12));
    badSpeed.speed = qInf();
    QVERIFY(!plan(badSpeed).ok());

    Request badSeparation = columnPair(at(0, -10), 0, at(0, -12));
    badSeparation.minSeparation = qInf();
    QVERIFY(!plan(badSeparation).ok());

    Request badObstacle = columnPair(at(0, -10), 0, at(0, -12));
    badObstacle.obstacles = {{3, QGeoCoordinate()}};
    QVERIFY(!plan(badObstacle).ok());
}

void FormationLegPlannerTest::_trackerSynchronizesSpeeds()
{
    FormationLegTracker tracker;
    tracker.start(parallelLeg(), 0);
    auto commands = tracker.update({{1, at(0, 0)}, {2, at(0, 10)}}, {}, 0, 12);
    QCOMPARE(commands.size(), 2);
    for (const auto& command : commands) {
        QCOMPARE(command.speed, 5.0);
        tracker.markSent(command.id, command.speed);
    }

    // 1 is 4 m ahead of the consensus (s = 0.44 vs 0.4), 2 is 4 m behind (within the desynchronisation bound).
    commands = tracker.update({{1, at(44, 0)}, {2, at(36, 10)}}, {}, 5000, 12);
    QCOMPARE(commands.size(), 2);
    for (const auto& command : commands) {
        // Progress comes back through a spherical round trip; compare with a tolerance.
        QVERIFY(qAbs(command.speed - (command.id == 1 ? 3.0 : 7.0)) < 0.01);
        tracker.markSent(command.id, command.speed);
    }
    QVERIFY(qAbs(tracker.consensusProgress() - 0.4) < 0.001);
    QVERIFY(qAbs(tracker.deviationMeters() - 4.0) < 0.05);

    // Unchanged speeds are not resent.
    QVERIFY(tracker.update({{1, at(44, 0)}, {2, at(36, 10)}}, {}, 6000, 12).isEmpty());
    // 2 closes 2 m of its gap, so both corrections change by 0.5 m/s; 2 awaits an ACK and is skipped.
    const auto partial = tracker.update({{1, at(44, 0)}, {2, at(38, 10)}}, {2}, 7000, 12);
    QCOMPARE(partial.size(), 1);
    QCOMPARE(partial.first().id, 1);
    QVERIFY(qAbs(partial.first().speed - 3.5) < 0.01);
    QCOMPARE(tracker.state(), FormationLegTracker::State::Running);
}

void FormationLegPlannerTest::_trackerClampsSpeedToCeiling()
{
    // A slow leg (2 m/s base) with member 1 4 m ahead of the consensus and member 2 4 m behind
    // (s = 0.44 / 0.36 vs 0.4): the gain asks for 0 m/s and 4 m/s, both outside the allowed band,
    // while the deviation stays within the desynchronisation bound.
    Leg slow = parallelLeg();
    slow.durationSec = 50;
    for (LegMember& entry : slow.members)
        entry.speed = 2;
    const auto speedFor = [](const QList<FormationLegTracker::Command>& commands, int id) {
        const auto it = std::find_if(commands.cbegin(), commands.cend(), [id](const auto& c) { return c.id == id; });
        return it == commands.cend() ? qQNaN() : it->speed;
    };

    FormationLegTracker capped;
    capped.start(slow, 0);
    for (const auto& command : capped.update({{1, at(0, 0)}, {2, at(0, 10)}}, {}, 0, 4))
        capped.markSent(command.id, command.speed);
    const auto cappedCommands = capped.update({{1, at(44, 0)}, {2, at(36, 10)}}, {}, 5000, 4);
    QVERIFY(qAbs(speedFor(cappedCommands, 2) - 3.6) < 0.01);  // 0.9 * maxSpeed(4)
    QVERIFY(qAbs(speedFor(cappedCommands, 1) - 1.0) < 0.01);  // floor: 0.5 * base

    // With an unknown max speed, the fallback ceiling is max(1.5 * base, base + 1) = 3 for a 2 m/s base.
    FormationLegTracker unknownMax;
    unknownMax.start(slow, 0);
    for (const auto& command : unknownMax.update({{1, at(0, 0)}, {2, at(0, 10)}}, {}, 0, qQNaN()))
        unknownMax.markSent(command.id, command.speed);
    const auto fallbackCommands = unknownMax.update({{1, at(44, 0)}, {2, at(36, 10)}}, {}, 5000, qQNaN());
    QVERIFY(qAbs(speedFor(fallbackCommands, 2) - 3.0) < 0.01);
}

void FormationLegPlannerTest::_trackerCompletesAndTimesOut()
{
    FormationLegTracker arriving;
    arriving.start(parallelLeg(), 0);
    QVERIFY(arriving.update({{1, at(99.5, 0)}, {2, at(100, 10.5)}}, {}, 21000, 12).isEmpty());
    QCOMPARE(arriving.state(), FormationLegTracker::State::Completed);

    FormationLegTracker stuck;
    stuck.start(parallelLeg(), 0);
    stuck.update({{1, at(0, 0)}, {2, at(0, 10)}}, {}, 60000, 12);
    QCOMPARE(stuck.state(), FormationLegTracker::State::Running);  // exactly 2T + 20 s is not yet a timeout
    QVERIFY(stuck.update({{1, at(0, 0)}, {2, at(0, 10)}}, {}, 60001, 12).isEmpty());
    QCOMPARE(stuck.state(), FormationLegTracker::State::TimedOut);
    QCOMPARE(stuck.reason(), QStringLiteral("航段超时：1、2 号机未到位"));
}

void FormationLegPlannerTest::_trackerContinuesWithoutRemovedMember()
{
    FormationLegTracker tracker;
    tracker.start(parallelLeg(), 0);
    tracker.removeMember(2);
    tracker.update({{1, at(100, 0)}}, {}, 20000, 12);
    QCOMPARE(tracker.state(), FormationLegTracker::State::Completed);

    FormationLegTracker rejected;
    rejected.start(parallelLeg(), 0);
    rejected.abort(QStringLiteral("2 号机拒绝航段指令"));
    rejected.abort(QStringLiteral("ignored once finished"));
    QCOMPARE(rejected.state(), FormationLegTracker::State::Aborted);
    QCOMPARE(rejected.reason(), QStringLiteral("2 号机拒绝航段指令"));
    QVERIFY(rejected.update({{1, at(0, 0)}, {2, at(0, 10)}}, {}, 1000, 12).isEmpty());
}

void FormationLegPlannerTest::_trackerRestartWithOwnLegKeepsMembers()
{
    FormationLegTracker tracker;
    tracker.start(parallelLeg(), 0);
    tracker.start(tracker.leg(), 1000);  // restarting with its own leg must not clear itself
    QCOMPARE(tracker.leg().members.size(), 2);
}

void FormationLegPlannerTest::_trackerSkipsArrivedMemberWhileAnotherLags()
{
    FormationLegTracker tracker;
    tracker.start(parallelLeg(), 0);
    for (const auto& command : tracker.update({{1, at(0, 0)}, {2, at(0, 10)}}, {}, 0, 12))
        tracker.markSent(command.id, command.speed);

    // Member 1 is within the arrival tolerance; member 2 still has about 10 m left (both 4.6 m from the
    // consensus, within the desynchronisation bound) and would otherwise pull member 1's speed command back
    // out via the consensus gain.
    const auto commands = tracker.update({{1, at(99.6, 0)}, {2, at(90.4, 10)}}, {}, 10000, 12);
    QVERIFY(std::none_of(commands.cbegin(), commands.cend(), [](const auto& c) { return c.id == 1; }));
    QCOMPARE(tracker.state(), FormationLegTracker::State::Running);
}

void FormationLegPlannerTest::_trackerExcludesNegligibleMemberFromConsensus()
{
    Leg leg;
    leg.durationSec = 20;
    // Member 2's hop is a negligible 0.2 m horizontal move (< kNegligibleDistance), with speed 0 ("let the
    // autopilot use its default speed") exactly like the planner emits for such members.
    leg.members = {{1, at(0, 0), at(100, 0), 100, 5}, {2, at(0, 10), at(0, 10.2), 0.2, 0}};
    FormationLegTracker tracker;
    tracker.start(leg, 0);
    const auto initial = tracker.update({{1, at(0, 0)}, {2, at(0, 10)}}, {}, 0, 12);
    QCOMPARE(initial.size(), 2);
    const auto initialFor2 = std::find_if(initial.cbegin(), initial.cend(), [](const auto& c) { return c.id == 2; });
    QVERIFY(initialFor2 != initial.cend());
    QCOMPARE(initialFor2->speed, 0.0);
    for (const auto& command : initial)
        tracker.markSent(command.id, command.speed);

    // Only member 1 (s = 0.5) participates in the consensus; member 2 is pinned at progress 1.0 and,
    // having speed 0, gets no speed correction.
    const auto commands = tracker.update({{1, at(50, 0)}, {2, at(0, 10)}}, {}, 5000, 12);
    QVERIFY(std::none_of(commands.cbegin(), commands.cend(), [](const auto& c) { return c.id == 2; }));
    QVERIFY(qAbs(tracker.consensusProgress() - 0.5) < 0.001);
}

void FormationLegPlannerTest::_trackerArrivalRequiresVerticalMatch()
{
    FormationLegTracker tracker;
    tracker.start(parallelLeg(), 0);
    for (const auto& command : tracker.update({{1, at(0, 0)}, {2, at(0, 10)}}, {}, 0, 12))
        tracker.markSent(command.id, command.speed);

    // Both members are horizontally at their end, but member 2 is 2 m too high: not "arrived" yet.
    tracker.update({{1, at(100, 0)}, {2, at(100, 10, 2)}}, {}, 10000, 12);
    QCOMPARE(tracker.state(), FormationLegTracker::State::Running);
}

void FormationLegPlannerTest::_trackerIgnoresMemberWithInvalidAltitude()
{
    FormationLegTracker tracker;
    tracker.start(parallelLeg(), 0);
    QGeoCoordinate unmeasured = at(30, 10);
    unmeasured.setAltitude(qQNaN());

    // Member 2's altitude is unknown on the very first tick; it must not receive a command (not even
    // the first dispatch) based on that unmeasured state.
    const auto commands = tracker.update({{1, at(0, 0)}, {2, unmeasured}}, {}, 0, 12);
    QCOMPARE(commands.size(), 1);
    QCOMPARE(commands.first().id, 1);
}

void FormationLegPlannerTest::_trackerAbortsWhenDesynchronized()
{
    FormationLegTracker tracker;
    tracker.start(parallelLeg(), 0);
    for (const auto& command : tracker.update({{1, at(0, 0)}, {2, at(0, 10)}}, {}, 0, 12))
        tracker.markSent(command.id, command.speed);

    // Both halfway along their lines; member 2 is 4 m off its line, within the bound.
    tracker.update({{1, at(50, 0)}, {2, at(50, 14)}}, {}, 10000, 12);
    QCOMPARE(tracker.state(), FormationLegTracker::State::Running);
    QVERIFY(qAbs(tracker.deviationMeters() - 4.0) < 0.05);

    // 6 m off its line: pair separation was only checked for synchronized straight-line motion, so the leg ends.
    QVERIFY(tracker.update({{1, at(51, 0)}, {2, at(51, 16)}}, {}, 11000, 12).isEmpty());
    QCOMPARE(tracker.state(), FormationLegTracker::State::Aborted);
    QCOMPARE(tracker.reason(), QStringLiteral("编队偏离同步位置 6.0 米，航段已终止"));
}

void FormationLegPlannerTest::_missionWaypointsForFormation()
{
    const auto item = [](int sequence, MAV_CMD command, MAV_FRAME frame, double lat, double lon, double alt) {
        return new MissionItem(sequence, command, frame, 0, 0, 0, qQNaN(), lat, lon, alt, true, false);
    };
    // Like `item`, but also sets the hold time (param1) and autoContinue explicitly.
    const auto itemEx = [](int sequence, MAV_CMD command, MAV_FRAME frame, double holdSeconds, double lat, double lon,
                           double alt, bool autoContinue) {
        return new MissionItem(sequence, command, frame, holdSeconds, 0, 0, qQNaN(), lat, lon, alt, autoContinue,
                               false);
    };

    {
        QList<MissionItem*> items{item(0, MAV_CMD_NAV_TAKEOFF, MAV_FRAME_GLOBAL_RELATIVE_ALT, 47.397, 8.5455, 20),
                                  item(1, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT, 47.398, 8.5455, 25),
                                  item(2, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL, 47.398, 8.547, 540),
                                  item(3, MAV_CMD_NAV_RETURN_TO_LAUNCH, MAV_FRAME_MISSION, 0, 0, 0)};
        const auto cleanup = qScopeGuard([&items] { qDeleteAll(items); });
        bool trailingRecovery = false;
        QString reason;
        const QList<QGeoCoordinate> waypoints =
            SwarmMissionSnapshot::capture(items, false).formationWaypoints(500, trailingRecovery, reason);
        QCOMPARE(waypoints.size(), 2);
        QVERIFY(qAbs(waypoints[0].latitude() - 47.398) < 1e-9);
        QVERIFY(qAbs(waypoints[0].longitude() - 8.5455) < 1e-9);
        QCOMPARE(waypoints[0].altitude(), 525.0);
        QVERIFY(qAbs(waypoints[1].latitude() - 47.398) < 1e-9);
        QVERIFY(qAbs(waypoints[1].longitude() - 8.547) < 1e-9);
        QCOMPARE(waypoints[1].altitude(), 540.0);
        QVERIFY(trailingRecovery);

        items.insert(2, item(2, MAV_CMD_DO_CHANGE_SPEED, MAV_FRAME_MISSION, 0, 0, 0));
        QVERIFY(
            SwarmMissionSnapshot::capture(items, false).formationWaypoints(500, trailingRecovery, reason).isEmpty());
        QVERIFY2(reason.contains(QStringLiteral("不支持")), qPrintable(reason));
    }

    // A trailing NAV_LAND marks trailingRecovery just like NAV_RETURN_TO_LAUNCH does.
    {
        QList<MissionItem*> items{item(0, MAV_CMD_NAV_TAKEOFF, MAV_FRAME_GLOBAL_RELATIVE_ALT, 47.397, 8.5455, 20),
                                  item(1, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT, 47.398, 8.5455, 25),
                                  item(2, MAV_CMD_NAV_LAND, MAV_FRAME_GLOBAL_RELATIVE_ALT, 47.398, 8.5455, 0)};
        const auto cleanup = qScopeGuard([&items] { qDeleteAll(items); });
        bool trailingRecovery = false;
        QString reason;
        QVERIFY(
            !SwarmMissionSnapshot::capture(items, false).formationWaypoints(500, trailingRecovery, reason).isEmpty());
        QVERIFY(trailingRecovery);
    }

    // A route that ends on a plain waypoint resets trailingRecovery to false.
    {
        QList<MissionItem*> items{item(0, MAV_CMD_NAV_TAKEOFF, MAV_FRAME_GLOBAL_RELATIVE_ALT, 47.397, 8.5455, 20),
                                  item(1, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT, 47.398, 8.5455, 25)};
        const auto cleanup = qScopeGuard([&items] { qDeleteAll(items); });
        bool trailingRecovery = true;
        QString reason;
        QVERIFY(
            !SwarmMissionSnapshot::capture(items, false).formationWaypoints(500, trailingRecovery, reason).isEmpty());
        QVERIFY(!trailingRecovery);
    }

    // NAV_RETURN_TO_LAUNCH is only accepted as the last item; in the middle it is an unsupported item.
    {
        QList<MissionItem*> items{item(0, MAV_CMD_NAV_TAKEOFF, MAV_FRAME_GLOBAL_RELATIVE_ALT, 47.397, 8.5455, 20),
                                  item(1, MAV_CMD_NAV_RETURN_TO_LAUNCH, MAV_FRAME_MISSION, 0, 0, 0),
                                  item(2, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT, 47.398, 8.5455, 25)};
        const auto cleanup = qScopeGuard([&items] { qDeleteAll(items); });
        bool trailingRecovery = false;
        QString reason;
        QVERIFY(
            SwarmMissionSnapshot::capture(items, false).formationWaypoints(500, trailingRecovery, reason).isEmpty());
        QVERIFY2(reason.contains(QStringLiteral("不支持")), qPrintable(reason));
    }

    // NAV_TAKEOFF is only accepted as the first item; elsewhere it is an unsupported item.
    {
        QList<MissionItem*> items{item(0, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT, 47.397, 8.5455, 20),
                                  item(1, MAV_CMD_NAV_TAKEOFF, MAV_FRAME_GLOBAL_RELATIVE_ALT, 47.398, 8.5455, 25)};
        const auto cleanup = qScopeGuard([&items] { qDeleteAll(items); });
        bool trailingRecovery = false;
        QString reason;
        QVERIFY(
            SwarmMissionSnapshot::capture(items, false).formationWaypoints(500, trailingRecovery, reason).isEmpty());
        QVERIFY2(reason.contains(QStringLiteral("不支持")), qPrintable(reason));
    }

    // A terrain-relative waypoint is rejected specifically for its coordinate frame.
    {
        QList<MissionItem*> items{item(0, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_TERRAIN_ALT, 47.398, 8.5455, 25)};
        const auto cleanup = qScopeGuard([&items] { qDeleteAll(items); });
        bool trailingRecovery = false;
        QString reason;
        QVERIFY(
            SwarmMissionSnapshot::capture(items, false).formationWaypoints(500, trailingRecovery, reason).isEmpty());
        QVERIFY2(reason.contains(QStringLiteral("坐标系")), qPrintable(reason));
    }

    // Takeoff immediately followed by landing leaves no executable waypoint.
    {
        QList<MissionItem*> items{item(0, MAV_CMD_NAV_TAKEOFF, MAV_FRAME_GLOBAL_RELATIVE_ALT, 47.397, 8.5455, 20),
                                  item(1, MAV_CMD_NAV_LAND, MAV_FRAME_GLOBAL_RELATIVE_ALT, 47.397, 8.5455, 0)};
        const auto cleanup = qScopeGuard([&items] { qDeleteAll(items); });
        bool trailingRecovery = false;
        QString reason;
        QVERIFY(
            SwarmMissionSnapshot::capture(items, false).formationWaypoints(500, trailingRecovery, reason).isEmpty());
        QCOMPARE(reason, QStringLiteral("航线没有可执行的航点"));
    }

    // A waypoint with a hold time is rejected for lingering.
    {
        QList<MissionItem*> items{
            itemEx(0, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT, 5, 47.398, 8.5455, 25, true)};
        const auto cleanup = qScopeGuard([&items] { qDeleteAll(items); });
        bool trailingRecovery = false;
        QString reason;
        QVERIFY(
            SwarmMissionSnapshot::capture(items, false).formationWaypoints(500, trailingRecovery, reason).isEmpty());
        QVERIFY2(reason.contains(QStringLiteral("悬停")), qPrintable(reason));
    }

    // A waypoint that will not auto-continue is rejected.
    {
        QList<MissionItem*> items{
            itemEx(0, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT, 0, 47.398, 8.5455, 25, false)};
        const auto cleanup = qScopeGuard([&items] { qDeleteAll(items); });
        bool trailingRecovery = false;
        QString reason;
        QVERIFY(
            SwarmMissionSnapshot::capture(items, false).formationWaypoints(500, trailingRecovery, reason).isEmpty());
        QVERIFY2(reason.contains(QStringLiteral("非自动继续")), qPrintable(reason));
    }

    // A relative-altitude waypoint that resolves below Home - 0.5 m AMSL is rejected.
    {
        QList<MissionItem*> items{item(0, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT, 47.398, 8.5455, -2)};
        const auto cleanup = qScopeGuard([&items] { qDeleteAll(items); });
        bool trailingRecovery = false;
        QString reason;
        QVERIFY(
            SwarmMissionSnapshot::capture(items, false).formationWaypoints(500, trailingRecovery, reason).isEmpty());
        QVERIFY2(reason.contains(QStringLiteral("低于 Home")), qPrintable(reason));
    }

    // An absolute-altitude waypoint below Home - 0.5 m AMSL is rejected the same way.
    {
        QList<MissionItem*> items{item(0, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL, 47.398, 8.5455, 498)};
        const auto cleanup = qScopeGuard([&items] { qDeleteAll(items); });
        bool trailingRecovery = false;
        QString reason;
        QVERIFY(
            SwarmMissionSnapshot::capture(items, false).formationWaypoints(500, trailingRecovery, reason).isEmpty());
        QVERIFY2(reason.contains(QStringLiteral("低于 Home")), qPrintable(reason));
    }

    // An invalid (NaN) leader Home altitude is rejected up front, before inspecting any item.
    {
        QList<MissionItem*> items{item(0, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT, 47.398, 8.5455, 25)};
        const auto cleanup = qScopeGuard([&items] { qDeleteAll(items); });
        bool trailingRecovery = false;
        QString reason;
        QVERIFY(SwarmMissionSnapshot::capture(items, false)
                    .formationWaypoints(qQNaN(), trailingRecovery, reason)
                    .isEmpty());
        QCOMPARE(reason, QStringLiteral("长机 Home 高度无效"));
    }
}

UT_REGISTER_TEST(FormationLegPlannerTest, TestLabel::Unit)
