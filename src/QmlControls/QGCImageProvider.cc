#include "QGCImageProvider.h"

#include <QtGui/QFont>
#include <QtGui/QPainter>
#include <limits>

QGCImageProvider::QGCImageProvider()
    : QQuickImageProvider(QQmlImageProviderBase::ImageType::Image), _dummy(320, 240, QImage::Format_RGBA8888)
{
    // Dummy temporary image until something comes along
    _dummy.fill(Qt::black);
    QPainter painter(&_dummy);
    QFont f = painter.font();
    f.setPixelSize(20);
    painter.setFont(f);
    painter.setPen(Qt::white);
    painter.drawText(QRectF(0, 0, _dummy.width(), _dummy.height()), Qt::AlignCenter, QStringLiteral("Waiting..."));
    _images[0] = _dummy;
}

QGCImageProvider::~QGCImageProvider()
{
    // qCDebug(ImageProtocolManagerLog) << Q_FUNC_INFO << this;
}

QImage QGCImageProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize)
{
    Q_UNUSED(requestedSize);

    QImage image = _dummy;
    const QStringList url = id.split('/');
    if (url.size() == 2) {
        bool vehicleIdValid = false;
        bool indexValid = false;
        const uint vehicleId = url[0].toUInt(&vehicleIdValid);
        // The index only invalidates the QML image cache; it does not select a stored image.
        (void) url[1].toUInt(&indexValid);
        if (vehicleIdValid && indexValid && vehicleId <= std::numeric_limits<uint8_t>::max()) {
            image = _images.value(static_cast<uint8_t>(vehicleId), _dummy);
        }
    }
    if (size) {
        *size = image.size();
    }

    return image;
}
