#include "QGCImageProviderTest.h"

#include "QGCImageProvider.h"

void QGCImageProviderTest::_optionalSize()
{
    QGCImageProvider provider;
    QImage image(10, 20, QImage::Format_RGB32);
    image.fill(Qt::red);
    provider.setImage(image, 1);
    QCOMPARE(provider.requestImage(QStringLiteral("1/0"), nullptr, {}), image);
    QVERIFY(!provider.requestImage({}, nullptr, {}).isNull());
}

void QGCImageProviderTest::_invalidId_data()
{
    QTest::addColumn<QString>("id");
    QTest::newRow("empty") << QString();
    QTest::newRow("no-index") << QStringLiteral("1");
    QTest::newRow("empty-index") << QStringLiteral("1/");
    QTest::newRow("extra-slash") << QStringLiteral("/1/0");
    QTest::newRow("non-numeric-vehicle") << QStringLiteral("x/0");
    QTest::newRow("non-numeric-index") << QStringLiteral("1/x");
    QTest::newRow("negative-vehicle") << QStringLiteral("-1/0");
    QTest::newRow("wrapped-vehicle") << QStringLiteral("257/0");
    QTest::newRow("overflow-vehicle") << QStringLiteral("4294967296/0");
    QTest::newRow("unknown-vehicle") << QStringLiteral("2/0");
}

void QGCImageProviderTest::_invalidId()
{
    QFETCH(QString, id);
    QGCImageProvider provider;
    QImage image(10, 20, QImage::Format_RGB32);
    image.fill(Qt::red);
    provider.setImage(image, 1);
    const QImage fallback = provider.requestImage({}, nullptr, {});
    QSize size;
    const QImage result = provider.requestImage(id, &size, {});
    QCOMPARE(result, fallback);
    QCOMPARE(size, fallback.size());
}

void QGCImageProviderTest::_vehicleImages()
{
    QGCImageProvider provider;
    QImage first(10, 20, QImage::Format_RGB32);
    first.fill(Qt::red);
    first.setPixelColor(0, 0, Qt::blue);
    QImage last(30, 40, QImage::Format_RGB32);
    last.fill(Qt::green);
    provider.setImage(first, 1);
    provider.setImage(last, 255);
    QSize size;
    QCOMPARE(provider.requestImage(QStringLiteral("1/1000"), &size, {}), first.flipped(Qt::Vertical));
    QCOMPARE(size, first.size());
    QCOMPARE(provider.requestImage(QStringLiteral("255/0"), &size, {}), last);
    QCOMPARE(size, last.size());
}

UT_REGISTER_TEST(QGCImageProviderTest, TestLabel::Unit)
