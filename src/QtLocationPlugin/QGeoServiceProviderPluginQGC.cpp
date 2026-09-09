#include "QGeoServiceProviderPluginQGC.h"

#include <QtCore/QThread>
#include <QtQml/QQmlEngine>

#include "QGCLoggingCategory.h"
#include "QGeoTiledMappingManagerEngineQGC.h"

QGC_LOGGING_CATEGORY(QGeoServiceProviderFactoryQGCLog, "QtLocationPlugin.QGeoServiceProviderFactoryQGC")

QGeoServiceProviderFactoryQGC::QGeoServiceProviderFactoryQGC(QObject* parent) : QObject(parent)
{
    qCDebug(QGeoServiceProviderFactoryQGCLog) << this;
}

QGeoServiceProviderFactoryQGC::~QGeoServiceProviderFactoryQGC()
{
    qCDebug(QGeoServiceProviderFactoryQGCLog) << this;
}

QGeoCodingManagerEngine* QGeoServiceProviderFactoryQGC::createGeocodingManagerEngine(const QVariantMap& parameters,
                                                                                     QGeoServiceProvider::Error* error,
                                                                                     QString* errorString) const
{
    Q_UNUSED(parameters);
    if (error) {
        *error = QGeoServiceProvider::NotSupportedError;
    }
    if (errorString) {
        *errorString = QStringLiteral("Geocoding Not Supported");
    }

    return nullptr;
}

QGeoMappingManagerEngine* QGeoServiceProviderFactoryQGC::createMappingManagerEngine(const QVariantMap& parameters,
                                                                                    QGeoServiceProvider::Error* error,
                                                                                    QString* errorString) const
{
    if (error) {
        *error = QGeoServiceProvider::NoError;
    }
    if (errorString) {
        *errorString = "";
    }

    if (!m_engine || m_engine->thread() != QThread::currentThread()) {
        if (error) {
            *error = QGeoServiceProvider::LoaderError;
        }
        if (errorString) {
            *errorString = tr("A QML engine on the current thread is required for mapping");
        }
        qCWarning(QGeoServiceProviderFactoryQGCLog) << "Mapping requested without a QML engine on the current thread";
        return nullptr;
    }

    QNetworkAccessManager* networkManager = m_engine->networkAccessManager();

    return new QGeoTiledMappingManagerEngineQGC(parameters, error, errorString, networkManager, nullptr);
}

QGeoRoutingManagerEngine* QGeoServiceProviderFactoryQGC::createRoutingManagerEngine(const QVariantMap& parameters,
                                                                                    QGeoServiceProvider::Error* error,
                                                                                    QString* errorString) const
{
    Q_UNUSED(parameters);
    if (error) {
        *error = QGeoServiceProvider::NotSupportedError;
    }
    if (errorString) {
        *errorString = QStringLiteral("Routing Not Supported");
    }

    return nullptr;
}

QPlaceManagerEngine* QGeoServiceProviderFactoryQGC::createPlaceManagerEngine(const QVariantMap& parameters,
                                                                             QGeoServiceProvider::Error* error,
                                                                             QString* errorString) const
{
    Q_UNUSED(parameters);
    if (error) {
        *error = QGeoServiceProvider::NotSupportedError;
    }
    if (errorString) {
        *errorString = QStringLiteral("Place Not Supported");
    }

    return nullptr;
}
