#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>

class QTcpServer;
class QTcpSocket;
class SwarmManager;
class SwarmSituationLayerUITest;

/// Loopback-only acceptance interface for test-capable QGC builds.
///
/// The server remains disabled unless QGC_SWARM_ACCEPTANCE_TOKEN_FILE names a valid owner-only secret file.
/// Requests are newline-delimited JSON and must carry the same token. Production builds do not compile this class.
class SwarmAcceptanceServer : public QObject
{
    Q_OBJECT

    friend class SwarmSituationLayerUITest;

public:
    explicit SwarmAcceptanceServer(SwarmManager* manager);

    bool startFromEnvironment();
    void stop();

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
    QJsonObject _dispatchCounts;
    QJsonObject _executionResult;
    /// Most recent SwarmManager::swarmCommandFeedback message, queued ones included. goto/formation_route
    /// responses only ever observe the synchronously emitted ones: a queued transitFinished() message cannot
    /// land before the response that reads this field is already built and sent.
    QString _lastFeedback;
    /// Per-vehicle outcome of the most recent "upload_route" write ("pending"/"ok"/"failed"), surfaced in
    /// _status() so the SITL script can poll for completion instead of trusting the synchronous "ok" reply.
    QHash<int, QString> _routeUploads;
};
