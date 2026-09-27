#pragma once

#include <QObject>
#include <QRemoteObjectReplica>
#include <QString>

namespace fastecu::ui
{

// The remote utility peer as MainWindow sees it. DesktopComposition connects
// it to the remote utility replica: the wait, the state changes, and the
// log/progress mirror, which it drops while the replica is not valid.
class RemotePeer final : public QObject
{
    Q_OBJECT

  public:
    // Blocks until the peer's source is available. wait_requested is
    // direct-connected, so the wait runs inside this call.
    void wait_for_source()
    {
        emit wait_requested();
    }

  signals:
    void wait_requested();
    void log_window_message(QString message);
    void progress(int value);
    void stateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State old_state);
};

} // namespace fastecu::ui
