#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>

class QTcpServer;
class QTcpSocket;
class SwarmManager;

/// Loopback-only acceptance interface for test-capable QGC builds.
///
/// The server remains disabled unless QGC_SWARM_ACCEPTANCE_TOKEN contains at least 16 characters.
/// Requests are newline-delimited JSON and must carry the same token. Production builds do not compile this class.
class SwarmAcceptanceServer : public QObject
{
    Q_OBJECT

public:
    explicit SwarmAcceptanceServer(SwarmManager* manager);

    bool startFromEnvironment();

private slots:
    void _onNewConnection();
    void _onReadyRead();

private:
    QJsonObject _handleRequest(const QJsonObject& request);
    QJsonObject _status() const;
    QJsonObject _configurePX4SITL() const;
    QJsonObject _setFlightMode(const QJsonObject& request) const;
    QJsonObject _executeActions(const QJsonObject& request) const;
    QJsonObject _renderedItemCounts() const;
    void _writeResponse(QTcpSocket* socket, const QJsonObject& response) const;

    SwarmManager* _manager = nullptr;
    QTcpServer* _server = nullptr;
    QString _token;
    QHash<QTcpSocket*, QByteArray> _buffers;
};
