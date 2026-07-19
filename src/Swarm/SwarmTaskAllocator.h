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
    struct Node {
        int    id = 0;
        double x  = 0.0;   ///< 本地米坐标
        double y  = 0.0;
    };

    /// 拍卖分配（目标数须等于机数；返回 vehicleId → targetId）
    QMap<int, int> auctionAssign(const QList<Node> &vehicles, const QList<Node> &targets) const;

    /// 顺序基线（按 id 升序一一对应）
    QMap<int, int> sequentialAssign(const QList<Node> &vehicles, const QList<Node> &targets) const;

    /// 对比评测：{seq_total, seq_max, auction_total, auction_max}（米）
    QVariantMap compare(const QList<Node> &vehicles, const QList<Node> &targets) const;

    static double assignmentCost(const QMap<int, int> &assignment,
                                 const QList<Node> &vehicles, const QList<Node> &targets,
                                 double &maxCost);
};
