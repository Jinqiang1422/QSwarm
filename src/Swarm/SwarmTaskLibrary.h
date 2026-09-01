/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmTaskLibrary
 *
 * 任务模板库：常用多阶段任务以 JSON 模板形式存储，供人工面板与大模型直接引用。
 *
 *   - 模板目录：QStandardPaths::AppDataLocation/SwarmTemplates（*.json，可手工增删改）
 *   - 内置模板：首次启动自动落盘 4 套（区域搜索后返航 / 编队巡航后返航 /
 *     快速出动待命 / 全员返航降落），落盘后与用户模板同等待遇、可编辑
 *   - 模板参数：动作与完成条件中的 "${key}" 占位符在实例化时按参数表代入，
 *     参数带默认值与 min/max 限幅
 *   - 执行通道：实例化 → SwarmTaskController::parseStages（统一校验）→ startTask；
 *     大模型经 run_task_template 工具引用时走 AI 面板的人在回路确认
 *
 * 模板 JSON Schema：
 * {
 *   "name": "模板名（唯一键，AI 按名引用）",
 *   "description": "用途说明",
 *   "version": 1,
 *   "parameters": [ {"key":"altitude","label":"作业高度(米)","type":"number",
 *                    "default":30,"min":10,"max":120}, ... ],
 *   "stages": [ {"name":"阶段名","actions":[动作,...],
 *                "complete_when":{"kind":"...","min_altitude":"${key}",...}}, ... ]
 * }
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtQmlIntegration/QtQmlIntegration>

class SwarmManager;

class SwarmTaskLibrary : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("")

    Q_PROPERTY(QVariantList templates READ templates NOTIFY templatesChanged)
    Q_PROPERTY(QString templateDir READ templateDir CONSTANT)

public:
    explicit SwarmTaskLibrary(SwarmManager* swarmManager);
    ~SwarmTaskLibrary();

    QVariantList templates() const { return _templateList; }

    QString templateDir() const { return _templateDir; }

    /// 重新扫描模板目录
    Q_INVOKABLE void reload();

    /// 删除模板（内置模板不可删除）
    Q_INVOKABLE bool deleteTemplate(const QString& name);

    /// 保存模板（name 已存在则覆盖）
    Q_INVOKABLE bool saveTemplate(const QString& name, const QString& description, const QJsonArray& stages,
                                  const QJsonArray& parameters = QJsonArray());

    /// 在文件管理器中打开模板目录
    Q_INVOKABLE void openTemplateDir();

    /// 模板的参数表（QML 表单生成用）：[{key,label,type,default,min,max}, ...]
    Q_INVOKABLE QVariantList templateParameters(const QString& name) const;

    /// 实例化模板：参数代入 "${key}" 占位符，返回 stages JSON；失败时 error 非空
    QJsonArray instantiate(const QString& name, const QVariantMap& paramValues, QString& error) const;

    /// AI 上下文摘要：[{name, description, stage_count, tags, min_fleet, parameters:[{key,default}]}]
    QJsonArray contextSummary() const;

    /// 模板元信息（QML 显示）：{tags, min_fleet, required_payload, stage_count, schema_version}
    Q_INVOKABLE QVariantMap templateMeta(const QString& name) const;

    /// 面板直接执行模板（操作员点击即确认）；经统一校验后启动状态机
    Q_INVOKABLE bool runTemplate(const QString& name, const QVariantMap& paramValues, double mapCenterLat,
                                 double mapCenterLon);

signals:
    void templatesChanged();
    void libraryFeedback(const QString& message);

private:
    struct TaskTemplate
    {
        QString name;
        QString description;
        QString filePath;
        bool builtIn = false;
        int schemaVersion = 1;    ///< v2 起含 tags/min_fleet/required_payload
        QStringList tags;         ///< 分类标签（检索/AI 匹配用）
        int minFleet = 0;         ///< 最少在线机数（0=不限制）
        QString requiredPayload;  ///< 载荷要求（空=无要求）
        QJsonArray parameters;
        QJsonArray stages;
    };

    void _ensureDefaults();  ///< 内置模板首次落盘
    void _scan();            ///< 扫描目录重建列表
    const TaskTemplate* _find(const QString& name) const;
    static QJsonValue _substitute(const QJsonValue& value, const QJsonObject& params);

    SwarmManager* _swarmManager = nullptr;
    QString _templateDir;
    QList<TaskTemplate> _templates;
    QVariantList _templateList;  ///< QML 用摘要列表
};
