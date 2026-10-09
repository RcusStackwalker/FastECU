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
    explicit RemoteUtility(const QString& peer_address, QString password, QWebSocket *web_socket = nullptr,
                           QObject *parent = nullptr);
    ~RemoteUtility();

    QRemoteObjectReplica::State State(void) const;
    bool IsValid(void);

  public slots:
    // NOLINTBEGIN(readability-identifier-naming): Qt slots keep Qt's camelBack names
    bool sendLogWindowMessage(QString message);
    bool setProgressbarValue(int value);
    void ping(QString message);
    void websocketConnected(void);
    void waitForSource(void);
    // NOLINTEND(readability-identifier-naming)

  signals:
    // NOLINTBEGIN(readability-identifier-naming): Qt signals keep Qt's camelBack names
    void stateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State old_state);
    // NOLINTEND(readability-identifier-naming)

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
    void StartKeepalive(void);
    void StopKeepalive(void);
    void StartRemote(void);
    void StartOverNetwok(void);
    void StartLocal(void);
    void SendKeepalive(void);
    void SendAutoDiscoveryMessage();

    static constexpr int kHeartbeatInterval{0};
    static constexpr int kKeepaliveInterval{7000};
    static constexpr int kPingsSequentlyMissedLimit{5};

  private slots:
    // NOLINTBEGIN(readability-identifier-naming): Qt slots keep Qt's camelBack names
    void utilityRemoteStateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State old_state);
    // NOLINTEND(readability-identifier-naming)
};
