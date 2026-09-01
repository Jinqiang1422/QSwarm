/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmInputFilter（WP1.2 防注入过滤层）
 *
 * 进入大模型前的文本安全检查：
 *   - 提示词注入模式识别（中/英文，权重计分）：忽略先前指令、角色劫持、
 *     系统提示词套取、越狱/开发者模式、安全绕过等
 *   - 三级判定：clean（放行）/ suspicious（放行但告警+审计）/ blocked（拦截）
 *   - 文本清洗：零宽字符剔除、超长截断、异常重复串压缩
 *
 * 适用对象：外部信息（目标上报文本、第三方数据等，P3 感知接入后为主）与
 * 操作员输入（操作员可信但仍记录可疑模式，防误粘贴注入载荷）。
 *
 * 定位：本模块是纵深防御的第一道（基于正则黑名单的模式识别），无法穷尽注入变体
 * （同义改写可绕过），须与统一校验闸门（SwarmConstraintChecker）、自主分级
 * （SwarmAutonomy）并用，不可作为唯一安全防线。
 *
 * 仅依赖 QtCore，可脱离 QGC 独立单元测试（tests/cpp）。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

class SwarmInputFilter : public QObject
{
    Q_OBJECT

public:
    struct Verdict
    {
        QString level = QStringLiteral("clean");  ///< clean / suspicious / blocked
        QStringList matched;                      ///< 命中的模式描述
        QString sanitized;                        ///< 清洗后的文本
        int score = 0;                            ///< 累计权重分
        QString source;                           ///< 输入来源语义标注(operator/telemetry/external),供审计溯源
    };

    static SwarmInputFilter* instance();

    /// 检查文本。source 仅用于语义标注（operator / telemetry / external）。
    Verdict check(const QString& text, const QString& source = QStringLiteral("external")) const;

    static bool isBlocked(const Verdict& verdict) { return verdict.level == QStringLiteral("blocked"); }

    static bool isSuspicious(const Verdict& verdict) { return verdict.level == QStringLiteral("suspicious"); }

    /// 内置攻击样例（level => 预期判定），供单元测试与 CI 自检遍历
    static QList<QPair<QString, QString>> builtinAttackSamples();

private:
    explicit SwarmInputFilter(QObject* parent = nullptr);

    QString _sanitize(const QString& text) const;

    static SwarmInputFilter* _instance;
};
