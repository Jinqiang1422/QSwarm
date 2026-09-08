#include <QtCore/QFile>
#include <QtTest/QtTest>

#include "SwarmTunnelProtocol.h"

class tst_protocol : public QObject
{
    Q_OBJECT
private slots:
    void sharedVectors();
    void isolatesMessagesAndSources();
    void boundsIncompleteMessages();
    void rejectsCorruptAndDuplicateFrames();
};

void tst_protocol::sharedVectors()
{
    QFile file(QFINDTESTDATA("protocol_v2_vectors.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto vectors = QJsonDocument::fromJson(file.readAll()).object().value("vectors").toArray();
    for (const auto& value : vectors) {
        const auto vector = value.toObject();
        const auto frames = SwarmTunnel::encodeDatagrams(vector.value("body").toString().toUtf8(),
                                                         quint64(vector.value("session").toDouble()),
                                                         quint32(vector.value("message_id").toInt()));
        QCOMPARE(frames.size(), vector.value("frames").toArray().size());
        SwarmTunnel::Reassembly rx;
        QJsonObject result;
        for (int i = frames.size() - 1; i >= 0; --i) {
            QCOMPARE(frames[i].toHex(), vector.value("frames").toArray()[i].toString().toLatin1());
            const auto decoded = SwarmTunnel::parseDatagram(frames[i], rx, "a", 0);
            if (!decoded.isEmpty())
                result = decoded;
        }
        QVERIFY(!result.isEmpty());
        QCOMPARE(rx.bufferedBytes(), 0);
    }
}

void tst_protocol::isolatesMessagesAndSources()
{
    const QByteArray body = "{\"kind\":\"plan_meta\",\"data\":{\"value\":\"" + QByteArray(400, 'x') + "\"}}";
    const auto first = SwarmTunnel::encodeDatagrams(body, 10, 1);
    const auto second = SwarmTunnel::encodeDatagrams(body, 10, 2);
    SwarmTunnel::Reassembly rx;
    QVERIFY(SwarmTunnel::parseDatagram(first.first(), rx, "a", 0).isEmpty());
    for (int i = 1; i < second.size(); ++i)
        QVERIFY(SwarmTunnel::parseDatagram(second[i], rx, "a", 0).isEmpty());
    for (int i = 1; i < first.size(); ++i)
        QVERIFY(SwarmTunnel::parseDatagram(first[i], rx, "b", 0).isEmpty());
    QJsonObject completed;
    for (int i = 1; i < first.size(); ++i)
        completed = SwarmTunnel::parseDatagram(first[i], rx, "a", 0);
    QVERIFY(!completed.isEmpty());
}

void tst_protocol::boundsIncompleteMessages()
{
    SwarmTunnel::Reassembly rx;
    const QByteArray body = "{\"kind\":\"plan_meta\",\"data\":{\"value\":\"" + QByteArray(65000, 'x') + "\"}}";
    auto frame = SwarmTunnel::encodeDatagrams(body, 1, 1).first();
    for (int i = 0; i < 20000; ++i) {
        qToBigEndian(quint32(i), frame.data() + 11);
        QVERIFY(SwarmTunnel::parseDatagram(frame, rx, QString::number(i % 8), 0).isEmpty());
    }
    QCOMPARE(rx.messages.size(), 64);
    QVERIFY(rx.bufferedBytes() <= 4 * 1024 * 1024);
    QVERIFY(rx.rejected > 0);
    rx.expire(2000);
    QCOMPARE(rx.bufferedBytes(), 0);
    QCOMPARE(rx.messages.size(), 0);
    QVERIFY(SwarmTunnel::encodeDatagrams(QByteArray(65537, 'x'), 1, 1).isEmpty());
}

void tst_protocol::rejectsCorruptAndDuplicateFrames()
{
    const auto valid = SwarmTunnel::encodeDatagrams(R"({"kind":"heartbeat","data":{}})", 1, 1).first();
    SwarmTunnel::Reassembly rx;
    auto bad = valid;
    bad[bad.size() - 1] ^= 1;
    QVERIFY(SwarmTunnel::parseDatagram(bad, rx, "a", 0).isEmpty());
    QVERIFY(!SwarmTunnel::parseDatagram(valid, rx, "a", 0).isEmpty());
    QVERIFY(SwarmTunnel::parseDatagram(valid, rx, "a", 1).isEmpty());
    QCOMPARE(rx.duplicates, 1);
    for (int size = 0; size < 27; ++size)
        QVERIFY(SwarmTunnel::parseDatagram(valid.left(size), rx, "b", 0).isEmpty());
    auto version = valid;
    version[2] = 1;
    QVERIFY(SwarmTunnel::parseDatagram(version, rx, "b", 0).isEmpty());
}

QTEST_GUILESS_MAIN(tst_protocol)
#include "tst_protocol.moc"
