#include "SwarmPersistence.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

bool SwarmPersistence::saveTasks(const QString& path, const QList<TaskRecord>& tasks)
{
    QDir().mkpath(QFileInfo(path).absolutePath());  // 确保目录存在,避免依赖调用顺序
    QJsonArray arr;
    for (const TaskRecord& t : tasks) {
        arr.append(QJsonObject{
            {QStringLiteral("id"), t.id},
            {QStringLiteral("type"), t.type},
            {QStringLiteral("assignee"), t.assignee},
            {QStringLiteral("approval"), t.approvalStatus},
            {QStringLiteral("status"), t.status},
            {QStringLiteral("version"), t.version},
        });
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    return f.commit();
}

QList<SwarmPersistence::TaskRecord> SwarmPersistence::loadTasks(const QString& path, bool* recoveryPersisted)
{
    if (recoveryPersisted) {
        *recoveryPersisted = true;
    }

    QList<TaskRecord> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return out;
    }
    const QByteArray data = f.readAll();
    f.close();
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        return out;
    }
    bool recoveredRunningTask = false;
    for (const QJsonValue& v : doc.array()) {
        if (!v.isObject()) {
            continue;
        }
        const QJsonObject o = v.toObject();
        TaskRecord t;
        t.id = o.value(QStringLiteral("id")).toString();
        t.type = o.value(QStringLiteral("type")).toString();
        t.assignee = o.value(QStringLiteral("assignee")).toString();
        t.approvalStatus = o.value(QStringLiteral("approval")).toString();
        t.status = o.value(QStringLiteral("status")).toString();
        t.version = o.value(QStringLiteral("version")).toInt(1);
        if (t.status == QStringLiteral("RUNNING")) {
            t.status = QStringLiteral("PAUSED");
            recoveredRunningTask = true;
        }
        out.append(t);
    }
    if (recoveredRunningTask) {
        const bool persisted = saveTasks(path, out);
        if (recoveryPersisted) {
            *recoveryPersisted = persisted;
        }
    }
    return out;
}
