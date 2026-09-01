/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmPerceptionBridge（WP3.2/3.7 接收端）
 *
 * 感知服务（独立进程）→ QGC 的链路桥：
 *   UDP 帧 [payload_type:uint16be][payload]（payload[0]=seq，其余 JSON 分片）
 *   ──► TUNNEL JSON 重组（与 services/perception/swarm_msgs.py 同一契约）
 *   ──► track_batch / heartbeat 信号 ──► SwarmTrackFusion / COP / 事件检测
 *
 * 解析核心 parseDatagram()/Reassembly 仅依赖 QtCore，可脱离网络单元测试；
 * socket 部分为薄封装。真实部署中同一字节流可由 MAVLink TUNNEL(#385) 承载，
 * 解析路径完全一致。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtQmlIntegration/QtQmlIntegration>

#include "SwarmTunnelProtocol.h"

class QUdpSocket;

class SwarmPerceptionBridge : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("")

    Q_PROPERTY(bool listening READ listening NOTIFY listeningChanged)
    Q_PROPERTY(quint16 listenPort READ listenPort NOTIFY listeningChanged)
    Q_PROPERTY(bool serviceOnline READ serviceOnline NOTIFY heartbeatChanged)
    Q_PROPERTY(double heartbeatHz READ heartbeatHz NOTIFY heartbeatChanged)
    Q_PROPERTY(int lastTrackCount READ lastTrackCount NOTIFY tracksChanged)
    Q_PROPERTY(double wireLatencyMs READ wireLatencyMs NOTIFY tracksChanged)

public:
    explicit SwarmPerceptionBridge(QObject* parent = nullptr);
    ~SwarmPerceptionBridge();

    bool listening() const { return _listening; }

    quint16 listenPort() const { return _port; }

    bool serviceOnline() const;

    double heartbeatHz() const { return _heartbeatHz; }

    int lastTrackCount() const { return _lastTrackCount; }

    double wireLatencyMs() const { return _wireLatencyMs; }

    Q_INVOKABLE bool listen(quint16 port = 15600);
    Q_INVOKABLE void stop();

    // 协议解析核心见 SwarmTunnelProtocol.h（QtCore-only，单测直接驱动）；
    // 本类仅保留 socket 薄封装与信号分发。
    using Reassembly = SwarmTunnel::Reassembly;

    /// 兼容入口：等价 SwarmTunnel::parseDatagram
    static QJsonObject parseDatagram(const QByteArray& datagram, Reassembly& rx)
    {
        return SwarmTunnel::parseDatagram(datagram, rx);
    }

signals:
    void listeningChanged();
    void heartbeatChanged();
    void tracksChanged();
    /// 批量航迹到达（字段字典列表 + 量测时间戳 us）
    void trackBatchReceived(const QVariantList& tracks, qint64 timeUsec);
    /// 心跳到达 {tracks_active, fps_x10, camera_channels, status, time_usec}
    void heartbeatReceived(const QVariantMap& hb);

private slots:
    void _onReadyRead();

private:
    void _dispatch(const QJsonObject& msg);

    QUdpSocket* _socket = nullptr;
    bool _listening = false;
    quint16 _port = 15600;
    Reassembly _rx;

    qint64 _lastHeartbeatMonoMs = -1;
    double _heartbeatHz = 0.0;
    int _lastTrackCount = 0;
    double _wireLatencyMs = 0.0;
};
