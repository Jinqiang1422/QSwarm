#pragma once
#include <QtCore/QList>
#include <QtCore/QString>

// 任务态 JSON 快照读写(纯 QtCore,可独立单测)。原子写防损坏;
// 加载时恢复语义:RUNNING → PAUSED(重启不自动续飞,MVP #6)。租约不持久化。
class SwarmPersistence
{
public:
    struct TaskRecord {
        QString id;
        QString type;
        QString assignee;
        QString approvalStatus;    ///< PENDING / APPROVED / REJECTED
        QString status;            ///< DRAFT / RUNNING / PAUSED / SUCCEEDED / ...
        int     version = 1;
    };

    static bool saveTasks(const QString &path, const QList<TaskRecord> &tasks);
    static QList<TaskRecord> loadTasks(const QString &path);
};
