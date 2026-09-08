#include "SwarmPerceptionBridge.h"

#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtNetwork/QNetworkDatagram>
#include <QtNetwork/QUdpSocket>

#include "SwarmClock.h"

SwarmPerceptionBridge::SwarmPerceptionBridge(QObject* parent) : QObject(parent)
{
    _expiryTimer.setInterval(100);
    connect(&_expiryTimer, &QTimer::timeout, this, &SwarmPerceptionBridge::_expire);
}

SwarmPerceptionBridge::~SwarmPerceptionBridge()
{
    stop();
}

bool SwarmPerceptionBridge::listen(quint16 port, const QString& bindAddress, const QStringList& allowedSources)
{
    stop();
    const QHostAddress address(bindAddress);
    if (address.isNull() || (!address.isLoopback() && allowedSources.isEmpty()))
        return false;
    _allowedSources = allowedSources;
    _socket = new QUdpSocket(this);
    const bool ok = _socket->bind(address, port);
    if (!ok) {
        delete _socket;
        _socket = nullptr;
        return false;
    }
    connect(_socket, &QUdpSocket::readyRead, this, &SwarmPerceptionBridge::_onReadyRead);
    _port = _socket->localPort();
    _expiryTimer.start();
    _listening = true;
    emit listeningChanged();
    return true;
}

void SwarmPerceptionBridge::stop()
{
    ++_listenGeneration;
    _expiryTimer.stop();
    _drainScheduled = false;
    _rx = {};
    _sourceSessions.clear();
    _sourceHeartbeats.clear();
    _sourceActivity.clear();
    _lastHeartbeatMonoMs = -1;
    _lastTrackCount = 0;
    _heartbeatHz = 0;
    _wireLatencyMs = -1;
    if (_online) {
        _online = false;
        emit onlineChanged();
    }
    emit heartbeatChanged();
    emit tracksChanged();
    emit diagnosticsChanged();
    if (_socket) {
        _socket->close();
        _socket->deleteLater();
        _socket = nullptr;
    }
    _listening = false;
    emit listeningChanged();
}

bool SwarmPerceptionBridge::serviceOnline() const
{
    return _online;
}

QVariantMap SwarmPerceptionBridge::diagnostics() const
{
    return {{QStringLiteral("buffered_bytes"), _rx.bufferedBytes()},
            {QStringLiteral("contexts"), _rx.messages.size()},
            {QStringLiteral("rejected"), _rx.rejected},
            {QStringLiteral("expired"), _rx.expired},
            {QStringLiteral("duplicates"), _rx.duplicates}};
}

void SwarmPerceptionBridge::_expire()
{
    const auto now = SwarmClock::nowMs();
    _rx.expire(now);
    for (auto it = _sourceActivity.begin(); it != _sourceActivity.end();) {
        if (now - it.value() >= 10000) {
            _sourceSessions.remove(it.key());
            _sourceHeartbeats.remove(it.key());
            it = _sourceActivity.erase(it);
        } else
            ++it;
    }
    bool online = false;
    for (auto heartbeat : std::as_const(_sourceHeartbeats))
        online |= now - heartbeat < 3000;
    if (_online != online) {
        _online = online;
        if (!online)
            _heartbeatHz = 0;
        emit onlineChanged();
        emit heartbeatChanged();
    }
    emit diagnosticsChanged();
}

void SwarmPerceptionBridge::_onReadyRead()
{
    if (_drainScheduled)
        return;
    QElapsedTimer budget;
    budget.start();
    int count = 0;
    while (_socket && _socket->hasPendingDatagrams() && count++ < 128 && budget.elapsed() < 2) {
        const qint64 size = _socket->pendingDatagramSize();
        const auto packet = _socket->receiveDatagram(130);
        const auto sourceAddress = packet.senderAddress();
        if (size > 130 || (!_allowedSources.contains(sourceAddress.toString()) &&
                           !(_allowedSources.isEmpty() && sourceAddress.isLoopback()))) {
            ++_rx.rejected;
            continue;
        }
        const QString peer = sourceAddress.toString() + QLatin1Char(':') + QString::number(packet.senderPort());
        if (!_sourceSessions.contains(peer) && _sourceSessions.size() >= 8) {
            ++_rx.rejected;
            continue;
        }
        const QByteArray bytes = packet.data();
        const QJsonObject msg = SwarmTunnel::parseDatagram(bytes, _rx, peer);
        if (!msg.isEmpty()) {
            const quint64 session = qFromBigEndian<quint64>(bytes.constData() + 3);
            if (_sourceSessions.contains(peer) && _sourceSessions.value(peer) != session)
                _sourceHeartbeats.remove(peer);
            _sourceSessions[peer] = session;
            _sourceActivity[peer] = SwarmClock::nowMs();
            _stream = peer + QLatin1Char('|') + QString::number(session);
            if (msg.value(QStringLiteral("kind")) == QStringLiteral("heartbeat"))
                _sourceHeartbeats[peer] = SwarmClock::nowMs();
            _dispatch(msg);
        }
    }
    if (_socket && _socket->hasPendingDatagrams()) {
        _drainScheduled = true;
        const quint64 generation = _listenGeneration;
        QTimer::singleShot(0, this, [this, generation] {
            if (_listenGeneration != generation)
                return;
            _drainScheduled = false;
            _onReadyRead();
        });
    }
}

void SwarmPerceptionBridge::_dispatch(const QJsonObject& msg)
{
    const QString kind = msg.value(QStringLiteral("kind")).toString();
    const QJsonObject data = msg.value(QStringLiteral("data")).toObject();
    const qint64 nowMs = SwarmClock::nowMs();

    if (kind == QStringLiteral("heartbeat")) {
        if (_lastHeartbeatMonoMs > 0) {
            const double interval = (nowMs - _lastHeartbeatMonoMs) / 1000.0;
            if (interval > 0.05) {
                _heartbeatHz = 0.7 * _heartbeatHz + 0.3 * (1.0 / interval);
            }
        }
        _lastHeartbeatMonoMs = nowMs;
        if (!_online) {
            _online = true;
            emit onlineChanged();
        }
        QVariantMap hb;
        hb[QStringLiteral("tracks_active")] = data.value(QStringLiteral("tracks_active")).toInt();
        hb[QStringLiteral("fps_x10")] = data.value(QStringLiteral("fps_x10")).toInt();
        hb[QStringLiteral("camera_channels")] = data.value(QStringLiteral("camera_channels")).toInt();
        hb[QStringLiteral("status")] = data.value(QStringLiteral("status")).toInt();
        hb[QStringLiteral("time_usec")] = data.value(QStringLiteral("time_usec")).toDouble();
        emit heartbeatReceived(hb);
        emit heartbeatChanged();
    } else if (kind == QStringLiteral("track_batch")) {
        const QJsonArray arr = data.value(QStringLiteral("tracks")).toArray();
        QVariantList tracks;
        tracks.reserve(arr.count());
        for (const QJsonValue& v : arr) {
            auto track = v.toObject().toVariantMap();
            track[QStringLiteral("stream_id")] = _stream;
            tracks.append(track);
        }
        _lastTrackCount = tracks.count();
        const qint64 tUsec = qint64(data.value(QStringLiteral("time_usec")).toDouble());
        if (tUsec > 0) {  // 同主机/PTP 时有效
            _wireLatencyMs = (QDateTime::currentMSecsSinceEpoch() >= tUsec / 1000
                                  ? QDateTime::currentMSecsSinceEpoch() - tUsec / 1000.0
                                  : -1.0);
        }
        emit trackBatchReceived(tracks, tUsec);
        emit tracksChanged();
    } else if (kind == QStringLiteral("track")) {
        _lastTrackCount = 1;
        auto track = data.toVariantMap();
        track[QStringLiteral("stream_id")] = _stream;
        emit trackBatchReceived({track}, qint64(data.value(QStringLiteral("time_usec")).toDouble()));
        emit tracksChanged();
    }
}
