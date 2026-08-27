#pragma once
#include <QtCore/QList>
#include <QtCore/QString>

// 任务态 JSON 快照读写(纯 QtCore,可独立单测)。原子写防损坏;
// 加载时恢复语义:RUNNING → PAUSED(重启不自动续飞,MVP #6)。租约不持久化。
class SwarmPersistence
{
public:
    struct TaskRecord
    {
        QString id;
        QString type;
        QString assignee;
        QString approvalStatus;  ///< PENDING / APPROVED / REJECTED
        QString status;          ///< DRAFT / RUNNING / PAUSED / SUCCEEDED / ...
        int version = 1;
    };

    static bool saveTasks(const QString& path, const QList<TaskRecord>& tasks);
    /// 加载任务；发现 RUNNING 时归一为 PAUSED 并原子写回，避免后续启动重复恢复同一运行态。
    /// recoveryPersisted 在无需写回或写回成功时为 true。
    static QList<TaskRecord> loadTasks(const QString& path, bool* recoveryPersisted = nullptr);
};
