#pragma once

#include <QObject>
#include <QtRemoteObjects/qremoteobjectnode.h>
#include "src/platform/desktop/common/serial/websocket/websocketiodevice.h"
#include "src/platform/desktop/common/serial/qtrohelper.hpp"

// Forward declaration
class RemoteUtilityReplica;

class RemoteUtility : public QObject
{
    Q_OBJECT
  public:
    explicit RemoteUtility(const QString& peerAddress, QString password, QWebSocket *web_socket = nullptr,
                           QObject *parent = nullptr);
    ~RemoteUtility();

    QRemoteObjectReplica::State state(void) const;
    bool isValid(void);

  public slots:
    bool send_log_window_message(QString message);
    bool set_progressbar_value(int value);
    void ping(QString message);
    void websocket_connected(void);
    void waitForSource(void);

  signals:
    void stateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State oldState);

  private:
    QString peer_address_;
    QString password_;
    const QString autodiscovery_message_ = "FastECU_PTP_Autodiscovery";
    RemoteUtilityReplica *remote_utility_{};
    const QString remote_object_name_utility_ = "FastECU_Utility";
    const QString wss_path_ = "/" + remote_object_name_utility_;
    const QString web_socket_password_header_ = "fastecu-basic-password";
    QWebSocket *web_socket_;
    WebSocketIoDevice *socket_;
    QRemoteObjectNode node_;
    QTimer *keepalive_timer_;
    int pings_sequently_missed_ = 0;
    void start_keepalive(void);
    void stop_keepalive(void);
    void startRemote(void);
    void startOverNetwok(void);
    void startLocal(void);
    void send_keepalive(void);
    void sendAutoDiscoveryMessage();

    static constexpr int kHeartbeatInterval{0};
    static constexpr int kKeepaliveInterval{7000};
    static constexpr int kPingsSequentlyMissedLimit{5};

  private slots:
    void utilityRemoteStateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State oldState);
};
