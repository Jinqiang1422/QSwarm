#include "SwarmTaskAllocator.h"

#include <QtCore/QHash>
#include <QtCore/QtMath>
#include <limits>

namespace {

double nodeDist(const SwarmTaskAllocator::Node& a, const SwarmTaskAllocator::Node& b)
{
    return qSqrt(qPow(a.x - b.x, 2) + qPow(a.y - b.y, 2));
}

// 拍卖法：机 i 对所有目标的最优/次优净价值(净价值 = -成本 - 价格)
struct NetBid
{
    int target = -1;
    double best = 0.0;
    double second = 0.0;
};

NetBid findBestNetValue(int i, const QList<QList<double>>& cost, const QList<double>& price, int n)
{
    NetBid r;
    r.best = -std::numeric_limits<double>::max();
    r.second = -std::numeric_limits<double>::max();
    for (int j = 0; j < n; j++) {
        const double value = -cost.at(i).at(j) - price.at(j);
        if (value > r.best) {
            r.second = r.best;
            r.best = value;
            r.target = j;
        } else if (value > r.second) {
            r.second = value;
        }
    }
    return r;
}

// CBBA：机 i 能赢(负里程得分 > winBid[j])的最高得分任务;无则 task=-1
struct WinTask
{
    int task = -1;
    double score = 0.0;
};

WinTask bestWinnableTask(int i, const QList<SwarmTaskAllocator::Node>& V, const QList<SwarmTaskAllocator::Node>& T,
                         const QList<double>& winBid, int nt)
{
    WinTask r;
    r.score = -std::numeric_limits<double>::max();
    for (int j = 0; j < nt; j++) {
        const double s = -nodeDist(V.at(i), T.at(j));
        if (s > winBid.at(j) && s > r.score) {
            r.score = s;
            r.task = j;
        }
    }
    return r;
}

}  // namespace

QMap<int, int> SwarmTaskAllocator::sequentialAssign(const QList<Node>& vehicles, const QList<Node>& targets) const
{
    QMap<int, int> out;
    const int n = qMin(vehicles.count(), targets.count());
    for (int i = 0; i < n; i++) {
        out.insert(vehicles.at(i).id, targets.at(i).id);
    }
    return out;
}

QMap<int, int> SwarmTaskAllocator::auctionAssign(const QList<Node>& vehicles, const QList<Node>& targets) const
{
    // 拍卖法求最小总成本分配（对称情形：机数 == 目标数）
    const int n = qMin(vehicles.count(), targets.count());
    QMap<int, int> out;
    if (n <= 0) {
        return out;
    }
    if (vehicles.count() != targets.count()) {  // 契约要求对称；非对称时超出部分被忽略
        qWarning("SwarmTaskAllocator::auctionAssign 机数(%d)≠目标数(%d)，仅分配前 %d 个", int(vehicles.count()),
                 int(targets.count()), n);
    }
    if (n == 1) {  // 单目标短路：避免次优净价值缺失导致出价溢出为 inf
        out.insert(vehicles.first().id, targets.first().id);
        return out;
    }

    // 成本矩阵（欧氏距离）
    QList<QList<double>> cost(n, QList<double>(n, 0.0));
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            cost[i][j] = nodeDist(vehicles.at(i), targets.at(j));
        }
    }

    const double epsilon = 1.0 / (n + 1);
    QList<double> price(n, 0.0);   // 目标价格
    QList<int> owner(n, -1);       // 目标当前归属（机索引）
    QList<int> assignment(n, -1);  // 机索引 → 目标索引
    QList<int> unassigned;
    for (int i = 0; i < n; i++) {
        unassigned.append(i);
    }

    // 拍卖轮次：未分配机对最优净价值目标出价，目标价高者得
    int guard = 0;
    const int maxRounds = 100000;
    while (!unassigned.isEmpty() && guard++ < maxRounds) {
        const int i = unassigned.takeFirst();
        const NetBid b = findBestNetValue(i, cost, price, n);
        if (b.target < 0) {
            break;
        }
        price[b.target] = price[b.target] + (b.best - b.second) + epsilon;  // 出价
        if (owner[b.target] >= 0) {
            assignment[owner[b.target]] = -1;
            unassigned.append(owner[b.target]);  // 原得主重新竞争
        }
        owner[b.target] = i;
        assignment[i] = b.target;
    }
    if (!unassigned.isEmpty()) {  // 达最大轮次仍未分配完 = 病态输入,返回部分分配
        qWarning("SwarmTaskAllocator::auctionAssign 达最大轮次仍有 %d 机未分配（返回部分分配）",
                 int(unassigned.count()));
    }

    for (int i = 0; i < n; i++) {
        if (assignment[i] >= 0) {
            out.insert(vehicles.at(i).id, targets.at(assignment[i]).id);
        }
    }
    return out;
}

QMap<int, int> SwarmTaskAllocator::cbbaAssign(const QList<Node>& vehicles, const QList<Node>& targets) const
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
    if (na != nt) {  // 契约要求对称；一对一场景非对称时部分机/目标无法配对
        qWarning("SwarmTaskAllocator::cbbaAssign 机数(%d)≠目标数(%d)", int(na), int(nt));
    }

    const double kLarge = std::numeric_limits<double>::max();
    QList<double> winBid(nt, -kLarge);  // y:每任务当前最高投标(=负里程)
    QList<int> winner(nt, -1);          // z:每任务当前赢家(机索引)
    QList<int> assigned(na, -1);        // 每机赢得的任务索引(-1=无)

    int guard = 0;
    const int maxIter = na * nt + na + 10;
    bool changed = true;
    while (changed && guard++ < maxIter) {
        changed = false;
        for (int i = 0; i < na; i++) {
            if (assigned[i] >= 0) {
                continue;  // 已持有任务,不再投标(除非下轮被顶替释放)
            }
            const WinTask w = bestWinnableTask(i, vehicles, targets, winBid, nt);
            if (w.task < 0) {
                continue;
            }
            // consensus:更高投标顶替原赢家 → 原赢家释放该任务(下轮重投)
            if (winner[w.task] >= 0 && winner[w.task] != i) {
                assigned[winner[w.task]] = -1;
            }
            winBid[w.task] = w.score;
            winner[w.task] = i;
            assigned[i] = w.task;
            changed = true;
        }
    }
    if (guard > maxIter) {  // guard 耗尽 = 未收敛,返回当前(可能次优/不完整)分配
        qWarning("SwarmTaskAllocator::cbbaAssign 达最大迭代(%d)仍未收敛（返回当前分配）", int(maxIter));
    }

    for (int i = 0; i < na; i++) {
        if (assigned[i] >= 0) {
            out.insert(vehicles.at(i).id, targets.at(assigned[i]).id);
        }
    }
    return out;
}

namespace {

using AllocNode = SwarmTaskAllocator::Node;

// findBestBid 返回：选中任务 + 最优插入位置 + 得分
struct Bid
{
    int task = -1;
    int pos = 0;
    double score = 0.0;
};

// 多任务 CBBA 求解器：持有工作状态(y/z/bundle/path),把最优插入投标(findBestBid)、
// 级联释放(releaseFrom)、两阶段(buildBundles/consensus)拆成独立小方法,各自
// 圈复杂度低、职责单一。忠实 CBBA：bundle-building 贪心加最优插入任务,consensus
// 用 winning-bid(y)/winner(z),被更高投标顶替时释放该任务及 bundle 后续。
struct BundleSolver
{
    const QList<AllocNode>& V;
    const QList<AllocNode>& T;
    const int na;
    const int nt;
    const int cap;
    const double kLarge = std::numeric_limits<double>::max();
    QList<double> y;           // 每任务当前最高投标(=负里程增量)
    QList<int> z;              // 每任务当前赢家
    QList<QList<int>> bundle;  // 每机 bundle(加入顺序)
    QList<QList<int>> path;    // 每机 path(访问顺序,最优插入)

    BundleSolver(const QList<AllocNode>& v, const QList<AllocNode>& t, int capacity)
        : V(v),
          T(t),
          na(v.count()),
          nt(t.count()),
          cap(capacity),
          y(t.count(), -std::numeric_limits<double>::max()),
          z(t.count(), -1),
          bundle(v.count()),
          path(v.count())
    {}

    double dVT(int i, int j) const { return qSqrt(qPow(V.at(i).x - T.at(j).x, 2) + qPow(V.at(i).y - T.at(j).y, 2)); }

    double dTT(int j, int k) const { return qSqrt(qPow(T.at(j).x - T.at(k).x, 2) + qPow(T.at(j).y - T.at(k).y, 2)); }

    double pathCost(int i, const QList<int>& pth) const
    {
        if (pth.isEmpty()) {
            return 0.0;
        }
        double c = dVT(i, pth.first());
        for (int k = 1; k < pth.size(); k++) {
            c += dTT(pth.at(k - 1), pth.at(k));
        }
        return c;
    }

    // 为 agent i 找"能赢(得分 > y[j])且最优插入(里程增量最小)"的任务
    Bid findBestBid(int i) const
    {
        const double base = pathCost(i, path[i]);
        Bid best;
        best.score = -kLarge;
        for (int j = 0; j < nt; j++) {
            if (bundle[i].contains(j)) {
                continue;
            }
            double bestInc = kLarge;
            int pos = path[i].size();
            for (int p = 0; p <= path[i].size(); p++) {  // 最优插入位置
                QList<int> tmp = path[i];
                tmp.insert(p, j);
                const double inc = pathCost(i, tmp) - base;
                if (inc < bestInc) {
                    bestInc = inc;
                    pos = p;
                }
            }
            const double s = -bestInc;  // 里程增量越小得分越高
            if (s > y[j] && s > best.score) {
                best.score = s;
                best.task = j;
                best.pos = pos;
            }
        }
        return best;
    }

    // 释放机 ag 的 bundle 中 taskJ 及其后续(路径依赖破裂,后续全部重规划)
    void releaseFrom(int ag, int taskJ)
    {
        const int idx = bundle[ag].indexOf(taskJ);
        if (idx < 0) {
            return;
        }
        for (int k = idx; k < bundle[ag].size(); k++) {
            const int t = bundle[ag].at(k);
            if (z[t] == ag) {
                z[t] = -1;
                y[t] = -kLarge;
            }
            path[ag].removeOne(t);
        }
        bundle[ag] = bundle[ag].mid(0, idx);
    }

    // Phase 1: 每机贪心加"能赢"的最优插入任务
    bool buildBundles()
    {
        bool changed = false;
        for (int i = 0; i < na; i++) {
            while (bundle[i].size() < cap) {
                const Bid b = findBestBid(i);
                if (b.task < 0) {
                    break;
                }
                if (z[b.task] >= 0 && z[b.task] != i) {
                    releaseFrom(z[b.task], b.task);  // 顶替原赢家
                }
                bundle[i].append(b.task);
                path[i].insert(b.pos, b.task);
                y[b.task] = b.score;
                z[b.task] = i;
                changed = true;
            }
        }
        return changed;
    }

    // Phase 2: 释放被顶替任务(及 bundle 后续)
    bool consensus()
    {
        bool changed = false;
        for (int i = 0; i < na; i++) {
            for (int k = 0; k < bundle[i].size(); k++) {
                if (z[bundle[i].at(k)] != i) {
                    releaseFrom(i, bundle[i].at(k));
                    changed = true;
                    break;
                }
            }
        }
        return changed;
    }

    void solve()
    {
        int guard = 0;
        const int maxIter = (na * nt + na) * 2 + 20;
        bool changed = true;
        while (changed && guard++ < maxIter) {
            changed = false;
            changed |= buildBundles();
            changed |= consensus();
        }
    }
};

}  // namespace

QMap<int, QList<int>> SwarmTaskAllocator::cbbaAssignBundle(const QList<Node>& vehicles, const QList<Node>& targets,
                                                           int maxBundlePerAgent) const
{
    // 多任务 CBBA(每机 bundle 多目标)：委托 BundleSolver 完成两阶段收敛,
    // 本入口只做参数校验、构造求解器、导出结果(求解细节见上方 BundleSolver)。
    QMap<int, QList<int>> out;
    const int na = vehicles.count();
    const int nt = targets.count();
    if (na <= 0 || nt <= 0) {
        return out;
    }
    const int cap = (maxBundlePerAgent > 0) ? maxBundlePerAgent : qMax(1, (nt + na - 1) / na);

    BundleSolver solver(vehicles, targets, cap);
    solver.solve();

    for (int i = 0; i < na; i++) {
        if (!solver.path[i].isEmpty()) {
            QList<int> ids;
            for (const int j : solver.path[i]) {
                ids.append(targets.at(j).id);
            }
            out.insert(vehicles.at(i).id, ids);
        }
    }
    return out;
}

double SwarmTaskAllocator::assignmentCost(const QMap<int, int>& assignment, const QList<Node>& vehicles,
                                          const QList<Node>& targets, double& maxCost)
{
    QHash<int, const Node*> vById, tById;  // 预索引消除按 id 的 O(n²) 线性查找
    for (const Node& node : vehicles) {
        vById.insert(node.id, &node);
    }
    for (const Node& node : targets) {
        tById.insert(node.id, &node);
    }

    double total = 0.0;
    maxCost = 0.0;
    for (auto it = assignment.constBegin(); it != assignment.constEnd(); ++it) {
        const Node* v = vById.value(it.key(), nullptr);
        const Node* t = tById.value(it.value(), nullptr);
        if (!v || !t) {
            continue;
        }
        const double d = nodeDist(*v, *t);
        total += d;
        maxCost = qMax(maxCost, d);
    }
    return total;
}

QVariantMap SwarmTaskAllocator::compare(const QList<Node>& vehicles, const QList<Node>& targets) const
{
    double seqMax = 0.0, aucMax = 0.0, cbbaMax = 0.0;
    const double seqTotal = assignmentCost(sequentialAssign(vehicles, targets), vehicles, targets, seqMax);
    const double aucTotal = assignmentCost(auctionAssign(vehicles, targets), vehicles, targets, aucMax);
    const double cbbaTotal = assignmentCost(cbbaAssign(vehicles, targets), vehicles, targets, cbbaMax);
    return {
        {QStringLiteral("seq_total_m"), seqTotal},     {QStringLiteral("seq_max_m"), seqMax},
        {QStringLiteral("auction_total_m"), aucTotal}, {QStringLiteral("auction_max_m"), aucMax},
        {QStringLiteral("cbba_total_m"), cbbaTotal},   {QStringLiteral("cbba_max_m"), cbbaMax},
    };
}
