// This is an open source non-commercial project. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com

#pragma once

#include <QRemoteObjectPendingCallWatcher>

namespace qtrohelper
{
// Convert QVariant to scalar templates
template <typename T> T QvariantToScalar(QVariant v);

template <> inline long QvariantToScalar<long>(QVariant v)
{
    // QVariant stores integers as 64 bits; long is 32 bits on Windows. The
    // value was a long on the sending side, so it round-trips.
    return static_cast<long>(v.toLongLong());
}

template <> inline unsigned long QvariantToScalar<unsigned long>(QVariant v)
{
    return v.toULongLong();
}

template <> inline int QvariantToScalar<int>(QVariant v)
{
    return v.toInt();
}

template <> inline unsigned int QvariantToScalar<unsigned int>(QVariant v)
{
    return v.toUInt();
}

template <> inline unsigned char QvariantToScalar<unsigned char>(QVariant v)
{
    return v.toInt();
}

template <> inline bool QvariantToScalar<bool>(QVariant v)
{
    return v.toBool();
}

template <> inline QString QvariantToScalar<QString>(QVariant v)
{
    return v.toString();
}

template <> inline QByteArray QvariantToScalar<QByteArray>(QVariant v)
{
    return v.toByteArray();
}

template <> inline QStringList QvariantToScalar<QStringList>(QVariant v)
{
    return v.toStringList();
}

/*
 * Do syncronous call of remote object method
 *
 * Note that QRemoteObjectPendingReply is template class.
 * This template first type argument must be
 * class QRemoteObjectPendingReply<return_type>
 * Second type argument is returning type of remote object method
 * For example source class implementation is:
 *
 * class QtroRemote : public QtroRemoteSimpleSource
 *   {
 *       Q_OBJECT
 *   public:
 *       long someFunc(QString s) override;
 *   };
 *
 *   On replica:
 *
 *  QScopedPointer<QtroRemoteReplica> qtro_remote(
 *       node.acquire<QtroRemoteReplica>("source_name")
 *   );
 *
 *   Then type of 'qtro_remote->someFunc()' will be deduced as
 *   QRemoteObjectPendingReply<long>.
 *   Note that 'qtro_remote->someFunc()' return type is 'long'.
 *   And 'slot_sync(qtro_remote->someFunc("text"))' call deduces to
 *
 *   slot_sync<QRemoteObjectPendingReply<long>, long>(qtro_remote->someFunc("text"))
 */
template <template <typename> typename QRemoteObjectPendingReply, typename RetType>
RetType SlotSync(const QRemoteObjectPendingReply<RetType>& slot)
{
    QVariant r;
    QScopedPointer<QRemoteObjectPendingCallWatcher> watcher{new QRemoteObjectPendingCallWatcher(slot)};
    QObject::connect(
        watcher.data(), &QRemoteObjectPendingCallWatcher::finished, watcher.data(),
        [&](QRemoteObjectPendingCallWatcher *watch) { r = watch->returnValue(); }, Qt::DirectConnection);
    watcher->waitForFinished();
    return QvariantToScalar<RetType>(r);
}

} // namespace qtrohelper
