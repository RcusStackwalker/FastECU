// Copyright (C) 2019 Ford Motor Company
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR BSD-3-Clause

#pragma once

#include <QBuffer>
#include <QIODevice>
#include <QPointer>
#include <QtWebSockets/QtWebSockets>

class WebSocketIoDevice : public QIODevice
{
    Q_OBJECT
  public:
    WebSocketIoDevice(QWebSocket *web_socket, QObject *parent = nullptr);

  signals:
    // NOLINTBEGIN(readability-identifier-naming): Qt signals keep Qt's camelBack names
    void disconnected();
    // NOLINTEND(readability-identifier-naming)

    // QIODevice interface
  public:
    qint64 bytesAvailable() const override;
    bool isSequential() const override;
    void close() override;

  protected:
    qint64 readData(char *data, qint64 maxlen) override;
    qint64 writeData(const char *data, qint64 len) override;

  private:
    QPointer<QWebSocket> m_socket_;
    QByteArray m_buffer_;
};
