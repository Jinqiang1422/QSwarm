#include "SwarmAuditLogger.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRandomGenerator>
#include <QtCore/QStandardPaths>

SwarmAuditLogger* SwarmAuditLogger::_instance = nullptr;

SwarmAuditLogger* SwarmAuditLogger::instance()
{
    if (!_instance) {
        _instance = new SwarmAuditLogger(QCoreApplication::instance());
    }
    return _instance;
}

SwarmAuditLogger::SwarmAuditLogger(QObject* parent) : QObject(parent)
{
    _mono.start();
    _openSession();
}

QString SwarmAuditLogger::auditDir() const
{
    if (!_auditDirOverride.isEmpty()) {
        return _auditDirOverride;
    }
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/SwarmAudit");
}

QString SwarmAuditLogger::sessionFilePath() const
{
    return auditDir() + QStringLiteral("/session_%1.jsonl").arg(_sessionId);
}

void SwarmAuditLogger::setAuditDirForTesting(const QString& dir)
{
    QMutexLocker locker(&_mutex);
    if (_file) {
        _file->close();
        delete _file;
        _file = nullptr;
    }
    _auditDirOverride = dir;
    _eventCount = 0;
    _openSession();
}

void SwarmAuditLogger::_openSession()
{
    QDir().mkpath(auditDir());
    _sessionId = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")) + QStringLiteral("_") +
                 QString::number(QRandomGenerator::global()->generate() & 0xFFFF, 16);
    if (_file) {
        _file->close();
        delete _file;
        _file = nullptr;
    }
    const QString path = sessionFilePath();
    _file = new QFile(path, this);
    if (!_file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        qWarning() << "SwarmAuditLogger: 无法打开审计文件" << path;
        delete _file;
        _file = nullptr;
    }
}

void SwarmAuditLogger::_writeLine(const QJsonObject& obj)
{
    if (!_enabled || !_file) {
        return;
    }
    QMutexLocker locker(&_mutex);
    _file->write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    _file->write("\n");
    _file->flush();
    _eventCount++;
}

void SwarmAuditLogger::log(const QString& category, const QString& action, const QVariantMap& data)
{
    QJsonObject obj;
    obj[QStringLiteral("seq")] = _eventCount + 1;
    obj[QStringLiteral("ts_wall")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    obj[QStringLiteral("ts_mono_ms")] = QJsonValue(static_cast<qint64>(_mono.elapsed()));
    obj[QStringLiteral("session")] = _sessionId;
    obj[QStringLiteral("category")] = category;
    obj[QStringLiteral("action")] = action;
    if (!data.isEmpty()) {
        obj[QStringLiteral("data")] = QJsonObject::fromVariantMap(data);
    }
    _writeLine(obj);
}

// ---------------------------------------------------------------------------
// 语义化便捷接口
// ---------------------------------------------------------------------------

void SwarmAuditLogger::logUserInput(const QString& text, const QString& filterVerdict)
{
    log(QStringLiteral("dialog"), QStringLiteral("user_input"),
        {{QStringLiteral("text"), text.left(2000)}, {QStringLiteral("filter"), filterVerdict}});
}

void SwarmAuditLogger::logLlmRequest(const QString& model, bool functionCalling, int historyLen)
{
    log(QStringLiteral("dialog"), QStringLiteral("llm_request"),
        {{QStringLiteral("model"), model},
         {QStringLiteral("function_calling"), functionCalling},
         {QStringLiteral("history_len"), historyLen}});
}

void SwarmAuditLogger::logLlmOutput(const QString& replyText, const QStringList& toolCalls)
{
    log(QStringLiteral("dialog"), QStringLiteral("llm_output"),
        {{QStringLiteral("text"), replyText.left(2000)},
         {QStringLiteral("tool_calls"), toolCalls.join(QStringLiteral(", "))}});
}

void SwarmAuditLogger::logValidation(int inputCount, int acceptedCount, const QStringList& warnings)
{
    log(QStringLiteral("safety"), QStringLiteral("validation"),
        {{QStringLiteral("input_count"), inputCount},
         {QStringLiteral("accepted_count"), acceptedCount},
         {QStringLiteral("warnings"), warnings.join(QStringLiteral(" | "))}});
}

void SwarmAuditLogger::logPlanPending(const QStringList& summary, const QString& reason)
{
    log(QStringLiteral("safety"), QStringLiteral("plan_pending"),
        {{QStringLiteral("summary"), summary.join(QStringLiteral("\n"))}, {QStringLiteral("reason"), reason}});
}

void SwarmAuditLogger::logPlanConfirmed(const QStringList& summary)
{
    log(QStringLiteral("safety"), QStringLiteral("plan_confirmed"),
        {{QStringLiteral("summary"), summary.join(QStringLiteral("\n"))}});
}

void SwarmAuditLogger::logPlanRejected()
{
    log(QStringLiteral("safety"), QStringLiteral("plan_rejected"));
}

void SwarmAuditLogger::logActionDispatch(const QString& type, const QVariantMap& params, const QString& description)
{
    QVariantMap data = params;
    data[QStringLiteral("type")] = type;
    data[QStringLiteral("description")] = description;
    log(QStringLiteral("exec"), QStringLiteral("action_dispatch"), data);
}

void SwarmAuditLogger::logOperatorCommand(const QString& command, const QVariantMap& params)
{
    QVariantMap data = params;
    data[QStringLiteral("command")] = command;
    log(QStringLiteral("exec"), QStringLiteral("operator_command"), data);
}

void SwarmAuditLogger::logTaskEvent(const QString& event, const QVariantMap& data)
{
    log(QStringLiteral("task"), event, data);
}

void SwarmAuditLogger::logTemplateRun(const QString& name, const QVariantMap& params)
{
    log(QStringLiteral("task"), QStringLiteral("template_run"),
        {{QStringLiteral("name"), name}, {QStringLiteral("params"), params}});
}

void SwarmAuditLogger::logAutonomyChange(int level)
{
    log(QStringLiteral("safety"), QStringLiteral("autonomy_change"), {{QStringLiteral("level"), level}});
}

QString SwarmAuditLogger::exportSession(const QString& destDir)
{
    if (!_file) {
        return QString();
    }
    {
        QMutexLocker locker(&_mutex);
        _file->flush();
    }
    QDir().mkpath(destDir);
    const QString dest = destDir + QStringLiteral("/") + QFileInfo(sessionFilePath()).fileName();
    QFile::remove(dest);
    if (!QFile::copy(sessionFilePath(), dest)) {
        return QString();
    }
    return dest;
}
