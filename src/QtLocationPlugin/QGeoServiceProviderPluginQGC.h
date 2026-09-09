#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QtPlugin>
#include <QtLocation/QGeoServiceProviderFactory>
#include <QtQml/QQmlEngine>

class QGeoServiceProviderFactoryQGC : public QObject, public QGeoServiceProviderFactory
{
    Q_OBJECT
    Q_INTERFACES(QGeoServiceProviderFactory)
    Q_PLUGIN_METADATA(IID "org.qt-project.qt.geoservice.serviceproviderfactory/6.0" FILE "qgc_maps_plugin.json")

public:
    QGeoServiceProviderFactoryQGC(QObject* parent = nullptr);
    ~QGeoServiceProviderFactoryQGC();

    QGeoCodingManagerEngine* createGeocodingManagerEngine(const QVariantMap& parameters,
                                                          QGeoServiceProvider::Error* error,
                                                          QString* errorString) const final;
    QGeoMappingManagerEngine* createMappingManagerEngine(const QVariantMap& parameters,
                                                         QGeoServiceProvider::Error* error,
                                                         QString* errorString) const final;
    QGeoRoutingManagerEngine* createRoutingManagerEngine(const QVariantMap& parameters,
                                                         QGeoServiceProvider::Error* error,
                                                         QString* errorString) const final;
    QPlaceManagerEngine* createPlaceManagerEngine(const QVariantMap& parameters, QGeoServiceProvider::Error* error,
                                                  QString* errorString) const final;

    void setQmlEngine(QQmlEngine* engine) final { m_engine = engine; }

private:
    QPointer<QQmlEngine> m_engine;
};
