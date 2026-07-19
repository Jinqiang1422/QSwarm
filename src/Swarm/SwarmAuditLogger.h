/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmAuditLogger（WP1.1 审计追溯系统）
 *
 * 全量记录“操作员指令 → LLM 推理输出 → 校验结果 → 下发指令 → 执行反馈”链路日志：
 *   - 每会话一个 JSONL 文件（一行一事件），目录：AppDataLocation/SwarmAudit/
 *   - 事件字段：seq / ts_wall(UTC ISO ms) / ts_mono_ms(单调时钟) / session / category / action / data
 *   - 语义化便捷接口统一 category/action 命名，便于事后检索与回放复盘
 *
 * 设计说明：
 *   - 仅依赖 QtCore，可脱离 QGC 独立单元测试（tests/cpp，用 setAuditDirForTesting 重定向）。
 *   - 单例（首次使用创建，挂 QCoreApplication 下）。不直接暴露 QML——
 *     界面通过 SwarmManager 的包装接口（auditDir / exportAudit 等）访问，
 *     避免本类引入 QML 依赖而损失可测性。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QMutex>
#include <QtCore/QObject>
#include <QtCore/QVariantMap>

class SwarmAuditLogger : public QObject
{
    Q_OBJECT

public:
    /// 首次调用创建（挂 QCoreApplication 下，随应用退出销毁）
    static SwarmAuditLogger *instance();

    bool    enabled() const { return _enabled; }
    void    setEnabled(bool enabled) { _enabled = enabled; }

    QString auditDir() const;
    QString sessionId() const { return _sessionId; }
    QString sessionFilePath() const;
    int     eventCount() const { return _eventCount; }

    /// 仅测试用：重定向审计目录并开启全新会话
    void setAuditDirForTesting(const QString &dir);

    // ------------------------------------------------------------------
    // 通用事件
    // ------------------------------------------------------------------
    void log(const QString &category, const QString &action, const QVariantMap &data = QVariantMap());

    // ------------------------------------------------------------------
    // 语义化便捷接口（category/action 命名固定，便于检索回放）
    // ------------------------------------------------------------------
    void logUserInput(const QString &text, const QString &filterVerdict);
    void logLlmRequest(const QString &model, bool functionCalling, int historyLen);
    void logLlmOutput(const QString &replyText, const QStringList &toolCalls);
    void logValidation(int inputCount, int acceptedCount, const QStringList &warnings);
    void logPlanPending(const QStringList &summary, const QString &reason);
    void logPlanConfirmed(const QStringList &summary);
    void logPlanRejected();
    void logActionDispatch(const QString &type, const QVariantMap &params, const QString &description);
    void logOperatorCommand(const QString &command, const QVariantMap &params = QVariantMap());
    void logTaskEvent(const QString &event, const QVariantMap &data = QVariantMap());
    void logTemplateRun(const QString &name, const QVariantMap &params);
    void logAutonomyChange(int level);

    /// 导出当前会话文件到 destDir，返回导出文件完整路径；失败返回空串
    QString exportSession(const QString &destDir);

private:
    explicit SwarmAuditLogger(QObject *parent = nullptr);

    void _openSession();                        // 调用方负责线程安全上下文
    void _writeLine(const QJsonObject &obj);

    static SwarmAuditLogger *_instance;

    bool          _enabled = true;
    QString       _sessionId;
    QString       _auditDirOverride;            // 仅测试注入
    QFile        *_file = nullptr;
    QElapsedTimer _mono;                        // 单调时钟（事件排序不受系统时钟回拨影响）
    int           _eventCount = 0;
    QMutex        _mutex;
};
