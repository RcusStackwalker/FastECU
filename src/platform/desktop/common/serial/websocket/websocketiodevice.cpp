// This is an open source non-commercial project. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com

// Copyright (C) 2019 Ford Motor Company
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR BSD-3-Clause

#include "src/platform/desktop/common/serial/websocket/websocketiodevice.h"

WebSocketIoDevice::WebSocketIoDevice(QWebSocket *webSocket, QObject *parent) : QIODevice(parent), m_socket_(webSocket)
{
    open(QIODevice::ReadWrite);
    connect(webSocket, &QWebSocket::disconnected, this, &WebSocketIoDevice::disconnected);
    connect(webSocket, &QWebSocket::binaryMessageReceived, this,
            [this](const QByteArray& message)
            {
                m_buffer_.append(message);
                emit readyRead();
            });
    connect(webSocket, &QWebSocket::bytesWritten, this, &WebSocketIoDevice::bytesWritten);
}

qint64 WebSocketIoDevice::bytesAvailable() const
{
    return QIODevice::bytesAvailable() + m_buffer_.size();
}

bool WebSocketIoDevice::isSequential() const
{
    return true;
}

void WebSocketIoDevice::close()
{
    if (m_socket_)
    {
        m_socket_->close();
    }
}

qint64 WebSocketIoDevice::readData(char *data, qint64 maxlen)
{
    auto sz = std::min(maxlen, qint64(m_buffer_.size()));
    if (sz <= 0)
    {
        return sz;
    }
    memcpy(data, m_buffer_.constData(), size_t(sz));
    m_buffer_.remove(0, sz);
    return sz;
}

qint64 WebSocketIoDevice::writeData(const char *data, qint64 len)
{
    if (m_socket_)
    {
        return m_socket_->sendBinaryMessage(QByteArray{data, int(len)});
    }
    return -1;
}
