#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

#include "SwarmPersistence.h"

class TstPersistence : public QObject
{
    Q_OBJECT
private slots:
    void roundTrip();
    void runningRecoversAsPaused();
    void missingFileReturnsEmpty();
    void corruptFileReturnsEmpty();
};

static SwarmPersistence::TaskRecord mk(const QString& id, const QString& status)
{
    SwarmPersistence::TaskRecord r;
    r.id = id;
    r.type = "SURVEY";
    r.assignee = "1";
    r.approvalStatus = "APPROVED";
    r.status = status;
    r.version = 2;
    return r;
}

void TstPersistence::roundTrip()
{
    QTemporaryDir dir;
    const QString p = dir.path() + "/tasks.json";
    QVERIFY(SwarmPersistence::saveTasks(p, {mk("t1", "DRAFT"), mk("t2", "SUCCEEDED")}));
    const auto out = SwarmPersistence::loadTasks(p);
    QCOMPARE(out.count(), 2);
    QCOMPARE(out[0].id, QStringLiteral("t1"));
    QCOMPARE(out[0].version, 2);
    QCOMPARE(out[1].status, QStringLiteral("SUCCEEDED"));
}

void TstPersistence::runningRecoversAsPaused()
{
    QTemporaryDir dir;
    const QString p = dir.path() + "/tasks.json";
    SwarmPersistence::saveTasks(p, {mk("t1", "RUNNING")});
    bool recoveryPersisted = false;
    const auto out = SwarmPersistence::loadTasks(p, &recoveryPersisted);
    QVERIFY(recoveryPersisted);
    QCOMPARE(out[0].status, QStringLiteral("PAUSED"));

    QFile persistedFile(p);
    QVERIFY(persistedFile.open(QIODevice::ReadOnly));
    const QJsonArray persisted = QJsonDocument::fromJson(persistedFile.readAll()).array();
    QCOMPARE(persisted[0].toObject().value(QStringLiteral("status")).toString(), QStringLiteral("PAUSED"));
}

void TstPersistence::missingFileReturnsEmpty()
{
    QVERIFY(SwarmPersistence::loadTasks("/nonexistent/xx.json").isEmpty());
}

void TstPersistence::corruptFileReturnsEmpty()
{
    QTemporaryDir dir;
    const QString p = dir.path() + "/bad.json";
    QFile f(p);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{not json");
    f.close();
    QVERIFY(SwarmPersistence::loadTasks(p).isEmpty());
}

QTEST_GUILESS_MAIN(TstPersistence)
#include "tst_persistence.moc"
