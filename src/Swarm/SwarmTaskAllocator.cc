#include "SwarmTaskAllocator.h"

#include <QtCore/QtMath>

#include <limits>

QMap<int, int> SwarmTaskAllocator::sequentialAssign(const QList<Node> &vehicles,
                                                    const QList<Node> &targets) const
{
    QMap<int, int> out;
    const int n = qMin(vehicles.count(), targets.count());
    for (int i = 0; i < n; i++) {
        out.insert(vehicles.at(i).id, targets.at(i).id);
    }
    return out;
}

QMap<int, int> SwarmTaskAllocator::auctionAssign(const QList<Node> &vehicles,
                                                 const QList<Node> &targets) const
{
    // 拍卖法求最小总成本分配（对称情形：机数 == 目标数）
    const int n = qMin(vehicles.count(), targets.count());
    QMap<int, int> out;
    if (n <= 0) {
        return out;
    }

    // 成本矩阵（欧氏距离）
    QList<QList<double>> cost(n, QList<double>(n, 0.0));
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            const double dx = vehicles.at(i).x - targets.at(j).x;
            const double dy = vehicles.at(i).y - targets.at(j).y;
            cost[i][j] = qSqrt(dx * dx + dy * dy);
        }
    }

    const double epsilon = 1.0 / (n + 1);
    QList<double> price(n, 0.0);            // 目标价格
    QList<int>    owner(n, -1);             // 目标当前归属（机索引）
    QList<int>    assignment(n, -1);        // 机索引 → 目标索引
    QList<int>    unassigned;
    for (int i = 0; i < n; i++) {
        unassigned.append(i);
    }

    // 拍卖轮次：未分配机对最优净价值目标出价，目标价高者得
    int guard = 0;
    const int maxRounds = 100000;
    while (!unassigned.isEmpty() && guard++ < maxRounds) {
        const int i = unassigned.takeFirst();

        // 净价值 = -成本 - 价格；找最优与次优
        double bestValue = -std::numeric_limits<double>::max();
        double secondValue = -std::numeric_limits<double>::max();
        int bestTarget = -1;
        for (int j = 0; j < n; j++) {
            const double value = -cost[i][j] - price[j];
            if (value > bestValue) {
                secondValue = bestValue;
                bestValue   = value;
                bestTarget  = j;
            } else if (value > secondValue) {
                secondValue = value;
            }
        }
        if (bestTarget < 0) {
            break;
        }
        // 出价 = 当前价格 + (最优 - 次优) + epsilon
        const double bid = price[bestTarget] + (bestValue - secondValue) + epsilon;
        price[bestTarget] = bid;
        if (owner[bestTarget] >= 0) {
            const int prev = owner[bestTarget];
            assignment[prev] = -1;
            unassigned.append(prev);    // 原得主重新竞争
        }
        owner[bestTarget] = i;
        assignment[i] = bestTarget;
    }

    for (int i = 0; i < n; i++) {
        if (assignment[i] >= 0) {
            out.insert(vehicles.at(i).id, targets.at(assignment[i]).id);
        }
    }
    return out;
}

QMap<int, int> SwarmTaskAllocator::cbbaAssign(const QList<Node> &vehicles,
                                              const QList<Node> &targets) const
{
    // CBBA/CBAA 分布式一致性拍卖:移植自 zehuilu/CBBA-Python 的核心机制,
    // 适配一对一里程最小化(每机 bundle 容量 1)。全连接通信图下,各机本地贪心
    // 投标(bundle-building),被更高 winning-bid 顶替的机释放任务(bundle_remove
    // 的一对一退化),迭代至无变化(共识收敛)。得分取负里程(越近越优)。
    const int na = vehicles.count();
    const int nt = targets.count();
    QMap<int, int> out;
    if (na <= 0 || nt <= 0) {
        return out;
    }

    const double kLarge = std::numeric_limits<double>::max();
    QList<double> winBid(nt, -kLarge);   // y:每任务当前最高投标(=负里程)
    QList<int>    winner(nt, -1);         // z:每任务当前赢家(机索引)
    QList<int>    assigned(na, -1);       // 每机赢得的任务索引(-1=无)

    const auto score = [&](int i, int j) {
        const double dx = vehicles.at(i).x - targets.at(j).x;
        const double dy = vehicles.at(i).y - targets.at(j).y;
        return -qSqrt(dx * dx + dy * dy);
    };

    int guard = 0;
    const int maxIter = na * nt + na + 10;
    bool changed = true;
    while (changed && guard++ < maxIter) {
        changed = false;
        for (int i = 0; i < na; i++) {
            if (assigned[i] >= 0) {
                continue;   // 已持有任务,不再投标(除非下轮被顶替释放)
            }
            // bundle-building:选"能赢"(得分超过当前 winning-bid)的最高得分任务
            int bestJ = -1;
            double bestScore = -kLarge;
            for (int j = 0; j < nt; j++) {
                const double s = score(i, j);
                if (s > winBid[j] && s > bestScore) {
                    bestScore = s;
                    bestJ = j;
                }
            }
            if (bestJ < 0) {
                continue;
            }
            // consensus:更高投标顶替原赢家 → 原赢家释放该任务(下轮重投)
            if (winner[bestJ] >= 0 && winner[bestJ] != i) {
                assigned[winner[bestJ]] = -1;
            }
            winBid[bestJ] = bestScore;
            winner[bestJ] = i;
            assigned[i]   = bestJ;
            changed = true;
        }
    }

    for (int i = 0; i < na; i++) {
        if (assigned[i] >= 0) {
            out.insert(vehicles.at(i).id, targets.at(assigned[i]).id);
        }
    }
    return out;
}

double SwarmTaskAllocator::assignmentCost(const QMap<int, int> &assignment,
                                          const QList<Node> &vehicles,
                                          const QList<Node> &targets,
                                          double &maxCost)
{
    double total = 0.0;
    maxCost = 0.0;
    for (auto it = assignment.constBegin(); it != assignment.constEnd(); ++it) {
        const Node *v = nullptr, *t = nullptr;
        for (const Node &node : vehicles) { if (node.id == it.key())   { v = &node; break; } }
        for (const Node &node : targets)  { if (node.id == it.value()) { t = &node; break; } }
        if (!v || !t) {
            continue;
        }
        const double d = qSqrt(qPow(v->x - t->x, 2) + qPow(v->y - t->y, 2));
        total += d;
        maxCost = qMax(maxCost, d);
    }
    return total;
}

QVariantMap SwarmTaskAllocator::compare(const QList<Node> &vehicles,
                                        const QList<Node> &targets) const
{
    double seqMax = 0.0, aucMax = 0.0, cbbaMax = 0.0;
    const double seqTotal  = assignmentCost(sequentialAssign(vehicles, targets), vehicles, targets, seqMax);
    const double aucTotal  = assignmentCost(auctionAssign(vehicles, targets), vehicles, targets, aucMax);
    const double cbbaTotal = assignmentCost(cbbaAssign(vehicles, targets), vehicles, targets, cbbaMax);
    return {
        {QStringLiteral("seq_total_m"),     seqTotal},
        {QStringLiteral("seq_max_m"),       seqMax},
        {QStringLiteral("auction_total_m"), aucTotal},
        {QStringLiteral("auction_max_m"),   aucMax},
        {QStringLiteral("cbba_total_m"),    cbbaTotal},
        {QStringLiteral("cbba_max_m"),      cbbaMax},
    };
}
