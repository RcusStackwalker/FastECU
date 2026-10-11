#include "remote_utility.h"

#include <utility>
#include "rep_remote_utility_replica.h"

RemoteUtility::RemoteUtility(const QString& peer_address, QString password, QWebSocket *web_socket, QObject *parent)
    : QObject{parent}, peer_address_(peer_address), password_(std::move(password)),
      web_socket_(web_socket == nullptr ? new QWebSocket("", QWebSocketProtocol::VersionLatest, this) : web_socket),
      socket_(new WebSocketIoDevice(web_socket_, web_socket_)), keepalive_timer_(new QTimer(this))
{
    if (peer_address.startsWith("local:"))
    {
        StartLocal();
    }
    else
    {
        StartOverNetwok();
    }
    QObject::connect(remote_utility_, &RemoteUtilityReplica::stateChanged, this,
                     &RemoteUtility::utilityRemoteStateChanged);
}

RemoteUtility::~RemoteUtility()
{
}

void RemoteUtility::StartLocal(void)
{
    QString p = peer_address_ + remote_object_name_utility_;
    node_.connectToNode(QUrl(p));
    remote_utility_ = node_.acquire<RemoteUtilityReplica>(remote_object_name_utility_);
}

void RemoteUtility::StartOverNetwok()
{
    QSslConfiguration ssl_configuration;
    ssl_configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
    web_socket_->setSslConfiguration(ssl_configuration);
    // Start node when Web Socket will be up
    QObject::connect(web_socket_, &QWebSocket::connected, this, &RemoteUtility::websocketConnected);
    node_.setHeartbeatInterval(kHeartbeatInterval);
    QObject::connect(web_socket_, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error)
                     { qDebug() << this->metaObject()->className() << "startOverNetwok QWebSocket error:" << error; });
    // WebSocket over SSL
    QUrl url("wss://" + peer_address_);
    url.setPath(wss_path_);
    QNetworkRequest req;
    req.setRawHeader(web_socket_password_header_.toUtf8(), password_.toUtf8());
    req.setUrl(url);
    web_socket_->open(req);

    // Connect to source published with name
    remote_utility_ = node_.acquire<RemoteUtilityReplica>(remote_object_name_utility_);
    // Don't wait for replication here, it should be done from outside
}

void RemoteUtility::websocketConnected(void)
{
    node_.addClientSideConnection(socket_);
    SendAutoDiscoveryMessage();
}

void RemoteUtility::waitForSource(void)
{
    // Wait for replication
    while (!remote_utility_->waitForSource(1000))
    {
        SendAutoDiscoveryMessage();
        qDebug() << "RemoteUtility: Waiting for remote peer...";
    }
}

void RemoteUtility::SendAutoDiscoveryMessage()
{
    if (web_socket_->isValid())
    {
        web_socket_->sendTextMessage(autodiscovery_message_);
    }
}

bool RemoteUtility::sendLogWindowMessage(QString message)
{
    return qtrohelper::SlotSync(remote_utility_->send_log_window_message(std::move(message)));
}

bool RemoteUtility::setProgressbarValue(int value)
{
    return qtrohelper::SlotSync(remote_utility_->set_progressbar_value(value));
}

QRemoteObjectReplica::State RemoteUtility::State(void) const
{
    return remote_utility_->state();
}

void RemoteUtility::ping(QString message)
{
    // Using pointer because of async response
    QRemoteObjectPendingCallWatcher *watcher =
        new QRemoteObjectPendingCallWatcher(remote_utility_->ping(std::move(message)));
    QObject::connect(
        watcher, &QRemoteObjectPendingCallWatcher::finished, this,
        [this](QRemoteObjectPendingCallWatcher *watch)
        {
            // qDebug() << Q_FUNC_INFO << watch->returnValue().toString();
            // Clean to avoid memory leak
            delete watch;
            this->pings_sequently_missed_ = 0;
        },
        Qt::QueuedConnection);
}

void RemoteUtility::StartKeepalive(void)
{
    connect(keepalive_timer_, &QTimer::timeout, this, &RemoteUtility::SendKeepalive);
    keepalive_timer_->start(kKeepaliveInterval);
}

void RemoteUtility::SendKeepalive(void)
{
    if (pings_sequently_missed_ == kPingsSequentlyMissedLimit)
    {
        qDebug() << "Missed keepalives limit exceeded. Assume the client is disconnected.";
        emit stateChanged(QRemoteObjectReplica::Suspect, remote_utility_->state());
    }

    ping("ping");
    pings_sequently_missed_++;
}

void RemoteUtility::StopKeepalive(void)
{
    keepalive_timer_->stop();
}

bool RemoteUtility::IsValid(void)
{
    return remote_utility_->state() == QRemoteObjectReplica::Valid;
}

void RemoteUtility::utilityRemoteStateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State old_state)
{
    emit stateChanged(state, old_state);
    if (state == QRemoteObjectReplica::Valid)
    {
        qDebug() << "RemoteUtility remote connection established";
        if (!peer_address_.startsWith("local:"))
        {
            StartKeepalive();
            qDebug() << "RemoteUtility keepalive started";
        }
    }
    else if (old_state == QRemoteObjectReplica::Valid)
    {
        qDebug() << "RemoteUtility remote connection lost";
        if (keepalive_timer_->isActive())
        {
            StopKeepalive();
            qDebug() << "RemoteUtility keepalive stopped";
        }
    }
}
