/****************************************************************************
 *
 * P1 模块单元测试（WP1.6）—— 仅依赖 Qt6 Core，可脱离 QGC 独立构建运行：
 *   - SwarmAutonomy：L1/L2/L3 行为矩阵
 *   - SwarmInputFilter：注入样例遍历 + 清洗 + 阈值边界
 *   - SwarmAuditLogger：JSONL 落盘/字段完整性/事件计数/导出
 *
 ****************************************************************************/

#include <QtTest/QtTest>

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>

#include "SwarmAutonomy.h"
#include "SwarmAuditLogger.h"
#include "SwarmInputFilter.h"

class TestP1Modules : public QObject
{
    Q_OBJECT

private slots:
    // ---------------- SwarmAutonomy ----------------
    void autonomy_levelNames();
    void autonomy_l1_blocksAll();
    void autonomy_l2_dangerousNeedsConfirm();
    void autonomy_l2_safeNoConfirm();
    void autonomy_l3_whitelist();
    void autonomy_l3_neverAuto();

    // ---------------- SwarmInputFilter ----------------
    void filter_builtinSamples();
    void filter_sanitize();
    void filter_cleanText();

    // ---------------- SwarmAuditLogger ----------------
    void audit_writeAndReadBack();
    void audit_export();

private:
    static SwarmAction makeAction(const QString &type, bool dangerous)
    {
        SwarmAction a;
        a.type = type;
        a.dangerous = dangerous;
        return a;
    }
};

// ---------------------------------------------------------------------------
// SwarmAutonomy
// ---------------------------------------------------------------------------
void TestP1Modules::autonomy_levelNames()
{
    QCOMPARE(SwarmAutonomy::levelName(SwarmAutonomy::Level::L1Suggest),    QStringLiteral("L1 仅建议"));
    QCOMPARE(SwarmAutonomy::levelName(SwarmAutonomy::Level::L2Supervised), QStringLiteral("L2 监督执行"));
    QCOMPARE(SwarmAutonomy::levelName(SwarmAutonomy::Level::L3Bounded),    QStringLiteral("L3 限定域自主"));
    QVERIFY(SwarmAutonomy::aiExecutionBlocked(SwarmAutonomy::Level::L1Suggest));
    QVERIFY(!SwarmAutonomy::aiExecutionBlocked(SwarmAutonomy::Level::L2Supervised));
    QVERIFY(!SwarmAutonomy::aiExecutionBlocked(SwarmAutonomy::Level::L3Bounded));
}

void TestP1Modules::autonomy_l1_blocksAll()
{
    const QList<SwarmAction> plan = { makeAction(QStringLiteral("pause"), false) };   // 纯安全动作
    QString reason;
    QVERIFY(SwarmAutonomy::needsConfirmation(SwarmAutonomy::Level::L1Suggest,
                                             SwarmAutonomy::defaultL3AllowedTypes(), plan, &reason));
    QVERIFY(reason.contains(QStringLiteral("L1")));
}

void TestP1Modules::autonomy_l2_dangerousNeedsConfirm()
{
    const QList<SwarmAction> plan = { makeAction(QStringLiteral("takeoff"), true) };
    QString reason;
    QVERIFY(SwarmAutonomy::needsConfirmation(SwarmAutonomy::Level::L2Supervised,
                                             SwarmAutonomy::defaultL3AllowedTypes(), plan, &reason));
    QVERIFY(reason.contains(QStringLiteral("takeoff")));
}

void TestP1Modules::autonomy_l2_safeNoConfirm()
{
    const QList<SwarmAction> plan = { makeAction(QStringLiteral("pause"), false),
                                      makeAction(QStringLiteral("set_selection"), false) };
    QVERIFY(!SwarmAutonomy::needsConfirmation(SwarmAutonomy::Level::L2Supervised,
                                              SwarmAutonomy::defaultL3AllowedTypes(), plan));
    // 空计划任何档位都不需确认
    QVERIFY(!SwarmAutonomy::needsConfirmation(SwarmAutonomy::Level::L2Supervised,
                                              SwarmAutonomy::defaultL3AllowedTypes(), {}));
}

void TestP1Modules::autonomy_l3_whitelist()
{
    const QStringList allowed = { QStringLiteral("takeoff"), QStringLiteral("land") };
    const QList<SwarmAction> inDomain  = { makeAction(QStringLiteral("takeoff"), true) };
    const QList<SwarmAction> outDomain = { makeAction(QStringLiteral("rtl"), true) };

    QVERIFY(!SwarmAutonomy::needsConfirmation(SwarmAutonomy::Level::L3Bounded, allowed, inDomain));
    QString reason;
    QVERIFY(SwarmAutonomy::needsConfirmation(SwarmAutonomy::Level::L3Bounded, allowed, outDomain, &reason));
    QVERIFY(reason.contains(QStringLiteral("rtl")));
}

void TestP1Modules::autonomy_l3_neverAuto()
{
    // emergency_disarm 即使在白名单内也绝不自动执行
    QStringList allowed = SwarmAutonomy::defaultL3AllowedTypes();
    allowed.append(QStringLiteral("emergency_disarm"));
    const QList<SwarmAction> plan = { makeAction(QStringLiteral("emergency_disarm"), true) };
    QVERIFY(SwarmAutonomy::needsConfirmation(SwarmAutonomy::Level::L3Bounded, allowed, plan));
}

// ---------------------------------------------------------------------------
// SwarmInputFilter
// ---------------------------------------------------------------------------
void TestP1Modules::filter_builtinSamples()
{
    SwarmInputFilter *filter = SwarmInputFilter::instance();
    const auto samples = SwarmInputFilter::builtinAttackSamples();
    QVERIFY(samples.count() >= 8);
    for (const auto &sample : samples) {
        const SwarmInputFilter::Verdict v = filter->check(sample.first);
        if (v.level != sample.second)
            qWarning() << "样本判定不符:" << sample.first << "得到" << v.level
                       << "期望" << sample.second << "score" << v.score << "命中" << v.matched;
        QCOMPARE(v.level, sample.second);
    }
}

void TestP1Modules::filter_sanitize()
{
    SwarmInputFilter *filter = SwarmInputFilter::instance();
    // 零宽字符应被剔除
    const SwarmInputFilter::Verdict v1 = filter->check(QStringLiteral("全体起飞到30米"));
    QVERIFY(!v1.sanitized.contains(QChar(0x200B)));
    // 超长文本截断到 2000 字符级
    const QString longText = QStringLiteral("起飞").repeated(2000);
    const SwarmInputFilter::Verdict v2 = filter->check(longText);
    QVERIFY(v2.sanitized.size() <= 2010);
    // 重复串压缩
    const QString spam = QStringLiteral("a").repeated(500);
    const SwarmInputFilter::Verdict v3 = filter->check(spam);
    QVERIFY(v3.sanitized.count(QLatin1Char('a')) < 100);
}

void TestP1Modules::filter_cleanText()
{
    SwarmInputFilter *filter = SwarmInputFilter::instance();
    const SwarmInputFilter::Verdict v = filter->check(QStringLiteral("全体起飞到30米"));
    QCOMPARE(v.level,  QStringLiteral("clean"));
    QCOMPARE(v.score,  0);
    QVERIFY(v.matched.isEmpty());
}

// ---------------------------------------------------------------------------
// SwarmAuditLogger
// ---------------------------------------------------------------------------
void TestP1Modules::audit_writeAndReadBack()
{
    const QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());

    SwarmAuditLogger *audit = SwarmAuditLogger::instance();
    audit->setAuditDirForTesting(tmpDir.path());
    QCOMPARE(audit->auditDir(), tmpDir.path());

    audit->logUserInput(QStringLiteral("全体起飞到30米"), QStringLiteral("clean"));
    audit->logValidation(2, 2, {});
    audit->logActionDispatch(QStringLiteral("takeoff"),
                             {{QStringLiteral("altitude"), 30.0}}, QStringLiteral("集群起飞至 30 米"));
    audit->logOperatorCommand(QStringLiteral("takeoffAll"),
                              {{QStringLiteral("altitude"), 30.0}});
    QCOMPARE(audit->eventCount(), 4);

    // 读回 JSONL 验证字段完整性
    QFile file(audit->sessionFilePath());
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QList<QByteArray> lines = file.readAll().split('\n');
    int validLines = 0;
    QStringList categories;
    for (const QByteArray &line : lines) {
        if (line.trimmed().isEmpty()) {
            continue;
        }
        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        QVERIFY(obj.contains(QStringLiteral("seq")));
        QVERIFY(obj.contains(QStringLiteral("ts_wall")));
        QVERIFY(obj.contains(QStringLiteral("ts_mono_ms")));
        QVERIFY(obj.contains(QStringLiteral("session")));
        QVERIFY(obj.contains(QStringLiteral("category")));
        QVERIFY(obj.contains(QStringLiteral("action")));
        categories.append(obj.value(QStringLiteral("category")).toString());
        validLines++;
    }
    QCOMPARE(validLines, 4);
    QVERIFY(categories.contains(QStringLiteral("dialog")));
    QVERIFY(categories.contains(QStringLiteral("safety")));
    QVERIFY(categories.contains(QStringLiteral("exec")));
}

void TestP1Modules::audit_export()
{
    const QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());
    SwarmAuditLogger *audit = SwarmAuditLogger::instance();
    audit->setAuditDirForTesting(tmpDir.path());
    audit->logTaskEvent(QStringLiteral("task_started"),
                        {{QStringLiteral("name"), QStringLiteral("测试任务")}});

    const QString exported = audit->exportSession(tmpDir.path() + QStringLiteral("/out"));
    QVERIFY(!exported.isEmpty());
    QVERIFY(QFile::exists(exported));
}

QTEST_GUILESS_MAIN(TestP1Modules)
#include "tst_p1modules.moc"
