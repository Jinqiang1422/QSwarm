#include "SwarmPersistence.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

bool SwarmPersistence::saveTasks(const QString &path, const QList<TaskRecord> &tasks)
{
    QDir().mkpath(QFileInfo(path).absolutePath());   // 确保目录存在,避免依赖调用顺序
    QJsonArray arr;
    for (const TaskRecord &t : tasks) {
        arr.append(QJsonObject{
            {QStringLiteral("id"),       t.id},
            {QStringLiteral("type"),     t.type},
            {QStringLiteral("assignee"), t.assignee},
            {QStringLiteral("approval"), t.approvalStatus},
            {QStringLiteral("status"),   t.status},
            {QStringLiteral("version"),  t.version},
        });
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    return f.commit();
}

QList<SwarmPersistence::TaskRecord> SwarmPersistence::loadTasks(const QString &path)
{
    QList<TaskRecord> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return out;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        return out;
    }
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        TaskRecord t;
        t.id             = o.value(QStringLiteral("id")).toString();
        t.type           = o.value(QStringLiteral("type")).toString();
        t.assignee       = o.value(QStringLiteral("assignee")).toString();
        t.approvalStatus = o.value(QStringLiteral("approval")).toString();
        t.status         = o.value(QStringLiteral("status")).toString();
        t.version        = o.value(QStringLiteral("version")).toInt(1);
        if (t.status == QStringLiteral("RUNNING")) {
            t.status = QStringLiteral("PAUSED");
        }
        out.append(t);
    }
    return out;
}
