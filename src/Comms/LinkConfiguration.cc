#include "LinkConfiguration.h"

#include "QGCLoggingCategory.h"
#ifndef QGC_NO_SERIAL_LINK
#include "SerialLink.h"
#endif
#include "BluetoothLink.h"
#include "LogReplayLink.h"
#include "TCPLink.h"
#include "UDPLink.h"
#ifdef QT_DEBUG
#include "MockLink.h"
#endif

QGC_LOGGING_CATEGORY(LinkConfigurationLog, "Comms.LinkConfiguration")

LinkConfiguration::LinkConfiguration(const QString& name, QObject* parent) : QObject(parent), _name(name)
{
    qCDebug(LinkConfigurationLog) << this;
}

LinkConfiguration::LinkConfiguration(const LinkConfiguration* copy, QObject* parent)
    : QObject(parent),
      _link(copy ? copy->_link : std::weak_ptr<LinkInterface>{}),
      _name(copy ? copy->name() : QString{}),
      _dynamic(copy && copy->isDynamic()),
      _autoConnect(copy && copy->isAutoConnect()),
      _highLatency(copy && copy->isHighLatency())
{
    qCDebug(LinkConfigurationLog) << this;

    if (!copy)
        qCWarning(LinkConfigurationLog) << "Cannot copy a null link configuration";
}

LinkConfiguration::~LinkConfiguration()
{
    qCDebug(LinkConfigurationLog) << this;
}

void LinkConfiguration::copyFrom(const LinkConfiguration* source)
{
    if (!source) {
        qCWarning(LinkConfigurationLog) << "Cannot copy a null link configuration";
        return;
    }

    setLink(source->_link.lock());
    setName(source->name());
    setDynamic(source->isDynamic());
    setAutoConnect(source->isAutoConnect());
    setHighLatency(source->isHighLatency());
}

LinkConfiguration* LinkConfiguration::createSettings(int type, const QString& name)
{
    LinkConfiguration* config = nullptr;

    switch (static_cast<LinkType>(type)) {
#ifndef QGC_NO_SERIAL_LINK
        case TypeSerial:
            config = new SerialConfiguration(name);
            break;
#endif
        case TypeUdp:
            config = new UDPConfiguration(name);
            break;
        case TypeTcp:
            config = new TCPConfiguration(name);
            break;
        case TypeBluetooth:
            config = new BluetoothConfiguration(name);
            break;
        case TypeLogReplay:
            config = new LogReplayConfiguration(name);
            break;
#ifdef QT_DEBUG
        case TypeMock:
            config = new MockConfiguration(name);
            break;
#endif
        case TypeLast:
        default:
            break;
    }

    return config;
}

LinkConfiguration* LinkConfiguration::duplicateSettings(const LinkConfiguration* source)
{
    if (!source) {
        qCWarning(LinkConfigurationLog) << "Cannot duplicate a null link configuration";
        return nullptr;
    }
    LinkConfiguration* dupe = nullptr;

    switch (source->type()) {
#ifndef QGC_NO_SERIAL_LINK
        case TypeSerial:
            dupe = new SerialConfiguration(qobject_cast<const SerialConfiguration*>(source));
            break;
#endif
        case TypeUdp:
            dupe = new UDPConfiguration(qobject_cast<const UDPConfiguration*>(source));
            break;
        case TypeTcp:
            dupe = new TCPConfiguration(qobject_cast<const TCPConfiguration*>(source));
            break;
        case TypeBluetooth:
            dupe = new BluetoothConfiguration(qobject_cast<const BluetoothConfiguration*>(source));
            break;
        case TypeLogReplay:
            dupe = new LogReplayConfiguration(qobject_cast<const LogReplayConfiguration*>(source));
            break;
#ifdef QT_DEBUG
        case TypeMock:
            dupe = new MockConfiguration(qobject_cast<const MockConfiguration*>(source));
            break;
#endif
        case TypeLast:
        default:
            break;
    }

    return dupe;
}

void LinkConfiguration::setName(const QString& name)
{
    if (name != _name) {
        _name = name;
        emit nameChanged(name);
    }
}

void LinkConfiguration::setLink(const SharedLinkInterfacePtr link)
{
    if (link.get() != this->link()) {
        _link = link;
        emit linkChanged();
        emit linkActiveChanged();

        if (link.get()) {
            (void) connect(link.get(), &LinkInterface::disconnected, this, &LinkConfiguration::linkChanged,
                           Qt::QueuedConnection);
        }
    }
}

void LinkConfiguration::setDynamic(bool dynamic)
{
    if (dynamic != _dynamic) {
        _dynamic = dynamic;
        emit dynamicChanged();
    }
}

void LinkConfiguration::setAutoConnect(bool autoc)
{
    if (autoc != _autoConnect) {
        _autoConnect = autoc;
        emit autoConnectChanged();
        emit linkActiveChanged();
    }
}

void LinkConfiguration::setHighLatency(bool hl)
{
    if (hl != _highLatency) {
        _highLatency = hl;
        emit highLatencyChanged();
    }
}
