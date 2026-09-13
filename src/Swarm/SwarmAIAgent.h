/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmAIAgent
 *
 * 大模型（LLM）集群任务接口：
 *   文本输入 → 任务理解 → 结构化动作规划 → 人工确认 → 分配执行
 *
 * 两种与模型对接的方式（useFunctionCalling 切换）：
 *   1. Function Calling（默认）：动作集以 OpenAI tools 定义下发，模型通过原生
 *      工具调用协议返回 tool_calls，格式漂移最小；
 *   2. JSON 模式（回退）：模型按系统提示词中的 Schema 输出 JSON 文本，
 *      解析端容错提取。用于不支持 tools 的端点/本地小模型。
 *
 * 动作执行不在这里实现——本类只负责“理解”，解析出的动作统一交给
 * SwarmManager::parseAndValidate() / executeActions()（与人工操作同一管线），
 * 多阶段任务则交给 SwarmTaskController 状态机编排。
 *
 * 人在回路：含危险动作的计划/多阶段任务默认挂起，操作员 confirmPlan() 后才执行。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtQmlIntegration/QtQmlIntegration>

#include "SwarmAction.h"
#include "SwarmTaskController.h"

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class SwarmManager;

class SwarmAIAgent : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("")

    Q_PROPERTY(QString apiBaseUrl READ apiBaseUrl WRITE setApiBaseUrl NOTIFY apiBaseUrlChanged)
    Q_PROPERTY(QString apiKey READ apiKey WRITE setApiKey NOTIFY apiKeyChanged)
    Q_PROPERTY(QString modelName READ modelName WRITE setModelName NOTIFY modelNameChanged)
    Q_PROPERTY(
        bool useFunctionCalling READ useFunctionCalling WRITE setUseFunctionCalling NOTIFY useFunctionCallingChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QStringList chatLog READ chatLog NOTIFY chatLogChanged)
    Q_PROPERTY(bool planPending READ planPending NOTIFY planPendingChanged)
    Q_PROPERTY(QStringList planSummary READ planSummary NOTIFY planChanged)
    Q_PROPERTY(bool planHasDangerous READ planHasDangerous NOTIFY planChanged)
    Q_PROPERTY(bool requireConfirm READ requireConfirm WRITE setRequireConfirm NOTIFY requireConfirmChanged)
    Q_PROPERTY(QVariantList candidates READ candidates NOTIFY candidatesChanged)
    Q_PROPERTY(double responseTimeSec READ responseTimeSec NOTIFY responseTimeSecChanged)
    /// 待确认计划各动作的参数 JSON 文本（WP2.3 计划编辑框初始值）
    Q_PROPERTY(QStringList pendingPlanParamsJson READ pendingPlanParamsJson NOTIFY planChanged)
    /// 待确认多阶段任务的结构化只读模型：各阶段名称/完成条件/动作列表（供逐项编辑 UI 使用）
    Q_PROPERTY(QVariantList pendingTaskModel READ pendingTaskModel NOTIFY planChanged)
    /// WP3.5：周期态势摘要开关与间隔（秒）
    Q_PROPERTY(bool situationSummaryEnabled READ situationSummaryEnabled WRITE setSituationSummaryEnabled NOTIFY
                   situationSummaryChanged)
    Q_PROPERTY(
        int situationIntervalSec READ situationIntervalSec WRITE setSituationIntervalSec NOTIFY situationSummaryChanged)

public:
    explicit SwarmAIAgent(SwarmManager* swarmManager);
    ~SwarmAIAgent();

    void shutdown();

    QString apiBaseUrl() const { return _apiBaseUrl; }

    void setApiBaseUrl(const QString& url);

    QString apiKey() const { return _apiKey; }

    void setApiKey(const QString& key);

    QString modelName() const { return _modelName; }

    void setModelName(const QString& model);

    bool useFunctionCalling() const { return _useFunctionCalling; }

    void setUseFunctionCalling(bool use);

    bool busy() const { return _busy; }

    QStringList chatLog() const { return _chatLog; }

    bool planPending() const { return !_pendingPlan.isEmpty() || _pendingTask; }

    QStringList planSummary() const;
    bool planHasDangerous() const;

    bool requireConfirm() const { return _requireConfirm; }

    void setRequireConfirm(bool require);

    /// 发送自然语言指令。mapCenterLat/Lon 由 QML 传入当前地图中心（供“当前区域”类指令参照）。
    Q_INVOKABLE void sendText(const QString& text, double mapCenterLat, double mapCenterLon);

    /// 确认执行挂起中的计划（动作序列或多阶段任务）
    Q_INVOKABLE void confirmPlan();

    /// 放弃挂起中的计划
    Q_INVOKABLE void rejectPlan();

    /// 清空对话历史（开始新任务会话）
    Q_INVOKABLE void clearHistory();
    void invalidatePendingWork();

    /// 测试接口连通性（GET {baseUrl}/models）
    Q_INVOKABLE void testConnection();

    QVariantList candidates() const { return _candidatesVariant; }

    double responseTimeSec() const { return _responseTimeSec; }

    QStringList pendingPlanParamsJson() const;

    QVariantList pendingTaskModel() const;

    /// 选择第 index 个候选方案（WP2.2 多候选），进入正常确认/执行流程
    Q_INVOKABLE void selectCandidate(int index);

    /// 放弃全部候选方案
    Q_INVOKABLE void rejectCandidates();

    /// 编辑待确认计划第 index 项动作的参数（WP2.3 计划可编辑），paramsJson 为参数 JSON 文本；
    /// 编辑后重新过统一校验，不通过则不生效
    Q_INVOKABLE void updatePendingAction(int index, const QString& paramsJson);

    /// 编辑待确认任务中某个阶段的某个动作参数；参数与原值合并后整体重校验。
    Q_INVOKABLE void updatePendingStageAction(int stageIndex, int actionIndex, const QString& paramsJson);

    /// 编辑待确认任务中某个阶段的完成条件；整个 complete_when 对象被替换后重校验。
    Q_INVOKABLE void updatePendingStageCondition(int stageIndex, const QString& conditionJson);

    /// WP3.5 态势问答：携带最新态势摘要向模型提问（事实须标注来源 [遥测]/[感知]/[推测]）
    Q_INVOKABLE void askSituation(const QString& question, double mapCenterLat, double mapCenterLon);

    /// WP3.6 联动：critical 告警触发 LLM 处置建议（异步，结果回对话区）
    Q_INVOKABLE void requestAdvice(const QString& alertText);

    bool situationSummaryEnabled() const { return _situationSummaryEnabled; }

    void setSituationSummaryEnabled(bool enabled);

    int situationIntervalSec() const { return _situationIntervalSec; }

    void setSituationIntervalSec(int sec);

#ifdef QGC_UNITTEST_BUILD
    /// 测试入口：直接喂入模型返回的动作数组，绕过网络请求。
    void handleActionsJsonForTest(const QJsonArray& actionsJson) { _handleActionsJson(actionsJson, QString()); }
#endif

signals:
    void apiBaseUrlChanged();
    void apiKeyChanged();
    void modelNameChanged();
    void useFunctionCallingChanged();
    void busyChanged();
    void chatLogChanged();
    void planPendingChanged();
    void planChanged();
    void requireConfirmChanged();
    void candidatesChanged();
    void responseTimeSecChanged();
    void situationSummaryChanged();
    /// 运行反馈（连接错误、解析失败、执行结果等）
    void agentFeedback(const QString& message);

private:
    QString _systemPromptJson() const;   ///< JSON 模式系统提示词（含输出格式约定）
    QString _systemPromptTools() const;  ///< Function Calling 系统提示词（仅规则，格式由 tools 承担）
    QJsonArray _toolsJson() const;       ///< OpenAI tools 定义（14 个动作/规划 + 4 个任务/候选）
    QJsonObject _buildSwarmContext(double mapCenterLat, double mapCenterLon) const;
    void _sendRequest();
    void _handleResponse(const QByteArray& body);
    void _handleActionsJson(const QJsonArray& actionsJson, const QString& replyText);
    bool _handleTaskJson(const QJsonObject& taskObj, QString& taskName, QList<SwarmTaskController::Stage>& stages,
                         QJsonArray& stagesJsonOut, QStringList& warnings);
    void _runTaskTemplate(const QJsonObject& args);
    void _handleCandidates(const QJsonObject& args);  ///< WP2.2 多候选方案
    void _queueOrExecute(const QList<SwarmAction>& plan);
    void _setBusy(bool busy);
    void _appendChat(const QString& who, const QString& text);
    void _sendSituationSummary();  ///< WP3.5 周期态势摘要

    /// 将解析后的阶段列表转换为 pendingTaskModel() 的只读形态；供该属性读取，也供
    /// _commitPendingStagesEdit 在提交前比较编辑是否为空操作复用，避免两处各写一份。
    static QVariantList _stagesToVariantList(const QList<SwarmTaskController::Stage>& stages);

    /// 提交编辑后的候选阶段 JSON：重跑 parseStages 校验通过、且与编辑前确有差异才落盘、
    /// 记审计、发通知；校验失败或与编辑前等价（如键名拼错被静默忽略）则如实反馈，不提交。
    bool _commitPendingStagesEdit(const QJsonArray& candidate, int stageIndex, int actionIndex, const QString& field,
                                  const QJsonObject& before, const QJsonObject& after, const QString& chatText);

    SwarmManager* _swarmManager = nullptr;
    QNetworkAccessManager* _nam = nullptr;
    QNetworkReply* _reply = nullptr;

    QString _apiBaseUrl;           // 默认 https://api.deepseek.com/v1（可在界面修改）
    QString _apiKey;
    QString _modelName;            // 默认 deepseek-chat
    bool _useFunctionCalling = true;
    bool _requireConfirm = false;  ///< 可选：连低风险动作也逐项确认；强制规则独立生效。
    bool _busy = false;

    QList<QJsonObject> _history;      ///< 多轮对话（不含 system），定期截断
    QList<SwarmAction> _pendingPlan;  ///< 待确认动作序列
    bool _pendingTask = false;
    QString _pendingTaskName;
    QList<SwarmTaskController::Stage> _pendingStages;  ///< 待确认多阶段任务
    QJsonArray _pendingStagesJson;                     ///< 与 _pendingStages 对应的 JSON 原文，编辑的作用对象
    QStringList _chatLog;
    double _lastMapCenterLat = 0.0;
    double _lastMapCenterLon = 0.0;

    /// WP2.2 候选方案（QML 展示于对比视图）
    struct Candidate
    {
        bool isTask = false;
        QList<SwarmAction> plan;                   ///< 动作型候选
        QList<SwarmTaskController::Stage> stages;  ///< 模板/任务型候选
        QJsonArray stagesJson;                     ///< 与 stages 对应的 JSON 原文
        QString taskName;
    };

    QList<Candidate> _candidates;
    QVariantList _candidatesVariant;

    double _responseTimeSec = 0.0;  ///< WP2.6：上次请求响应时延
    qint64 _requestStartMs = 0;

    // ---- WP3.5 态势摘要与问答 ----
    bool _situationSummaryEnabled = false;
    int _situationIntervalSec = 60;
    QTimer* _situationTimer = nullptr;
};
