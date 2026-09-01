/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmTaskAllocator（WP2.4 任务分配算法）
 *
 * 拍卖法（Bertsekas Auction）分配：N 架机 ↔ N 个目标（覆盖条带质心），
 * 最小化总里程成本；与 id 升序（顺序）基线可对比评测。
 * CBBA 分布式捆绑分配为后续增强（P4 动态场景）。
 *
 * 仅依赖 QtCore，可独立单元测试。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QList>
#include <QtCore/QMap>
#include <QtCore/QVariantMap>

class SwarmTaskAllocator
{
public:
    struct Node
    {
        int id = 0;
        double x = 0.0;  ///< 本地米坐标
        double y = 0.0;
    };

    /// 拍卖分配（目标数须等于机数；返回 vehicleId → targetId）
    QMap<int, int> auctionAssign(const QList<Node>& vehicles, const QList<Node>& targets) const;

    /// CBBA 分布式一致性拍卖(移植自 zehuilu/CBBA-Python 核心机制)。
    /// 全连接通信图模拟:各机本地贪心投标(bundle-building) + winning-bid/winner
    /// 共识解冲突(被更高投标顶替则释放),迭代至收敛;适配一对一里程最小化。
    /// 与集中式 auctionAssign 形成"集中/分布"双模,对应开发计划 P4。
    QMap<int, int> cbbaAssign(const QList<Node>& vehicles, const QList<Node>& targets) const;

    /// 多任务 CBBA:每机可分配多个目标(bundle),用于目标数 > 机数的场景。
    /// compute_bid 取"最优插入位置的里程增量",consensus 被更高投标顶替时释放
    /// 该任务及 bundle 后续(路径依赖破裂)。maxBundlePerAgent=0 时自动取 ceil(目标/机)
    /// 均衡分配。返回 机id → 按访问顺序的目标id列表。
    QMap<int, QList<int>> cbbaAssignBundle(const QList<Node>& vehicles, const QList<Node>& targets,
                                           int maxBundlePerAgent = 0) const;

    /// 顺序基线（按 id 升序一一对应）
    QMap<int, int> sequentialAssign(const QList<Node>& vehicles, const QList<Node>& targets) const;

    /// 对比评测：{seq_total, seq_max, auction_total, auction_max}（米）
    QVariantMap compare(const QList<Node>& vehicles, const QList<Node>& targets) const;

    static double assignmentCost(const QMap<int, int>& assignment, const QList<Node>& vehicles,
                                 const QList<Node>& targets, double& maxCost);
};
