/*
    SPDX-FileCopyrightText: 2024 KDE contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "greeteripcclient.h"

#include <Sonic/Logging/Log>

#include <QDataStream>
#include <QIODevice>

#include <Sonic/Ipc/Client>
#include <Sonic/Ipc/Connection>

namespace {
// Sonic IPC protocol version of the greeter socket; KSldApp's GreeterIpcServer ships with it.
constexpr quint16 greeterIpcProtocolVersion = 1;
}

GreeterIpcClient::GreeterIpcClient(QObject *parent)
    : QObject(parent)
    , m_nextMessageId(1)
{
}

GreeterIpcClient::~GreeterIpcClient()
{
    disconnectFromHost();
}

QString GreeterIpcClient::socketName()
{
    // The socket name is based on the server's process ID
    // The greeter receives this via command line argument or environment variable
    return QStringLiteral("kscreenlocker-greeter-ipc");
}

bool GreeterIpcClient::connectToHost()
{
    if (m_connection) {
        return true;
    }
    if (!m_client) {
        m_client = new Sonic::Ipc::Client(greeterIpcProtocolVersion, Sonic::Ipc::defaultMaxPayload, this);
        connect(m_client, &Sonic::Ipc::Client::connected, this, &GreeterIpcClient::handleConnected);
        connect(m_client, &Sonic::Ipc::Client::connectionFailed, this, [this](const QString &reason) {
            sonicCritical() << "GreeterIpcClient: Error" << reason;
            Q_EMIT connectionError(reason);
        });
    }

    // Get socket name from environment variable set by KSldApp
    QString socketName = QString::fromUtf8(qgetenv("KSCREENLOCKER_IPC_SOCKET"));
    if (socketName.isEmpty()) {
        // Fallback to default
        socketName = GreeterIpcClient::socketName();
    }

    m_client->connectToServer(socketName);

    return true;
}

void GreeterIpcClient::disconnectFromHost()
{
    m_connection = nullptr;
    if (m_client) {
        m_client->disconnectFromServer();
    }
}

bool GreeterIpcClient::isConnected() const
{
    return m_connection != nullptr;
}

void GreeterIpcClient::handleConnected(Sonic::Ipc::Connection *connection)
{
    m_connection = connection;
    connect(connection, &Sonic::Ipc::Connection::frameReceived, this, &GreeterIpcClient::handleFrame);
    connect(connection, &Sonic::Ipc::Connection::disconnected, this, [this, connection] {
        if (m_connection == connection) {
            m_connection = nullptr;
        }
        Q_EMIT disconnected();
    });
    Q_EMIT connected();
}

void GreeterIpcClient::handleFrame(quint32 frameType, const QByteArray &payload)
{
    QDataStream in(payload);

    quint32 messageType;
    quint32 messageId;
    in >> messageType >> messageId;
    if (messageType != frameType) {
        sonicWarning() << "GreeterIpcClient: frame type" << frameType << "doesn't match message type" << messageType;
        return;
    }

    switch (messageType) {
    case MsgResponse: {
        quint8 success;
        in >> success;
        auto it = m_pendingCallbacks.find(messageId);
        if (it != m_pendingCallbacks.end()) {
            it.value()(success != 0);
            m_pendingCallbacks.erase(it);
        }
        break;
    }
    case MsgScreenAdded: {
        QString screenName;
        QRect geometry;
        in >> screenName >> geometry;
        Q_EMIT screenAdded(screenName, geometry);
        break;
    }
    case MsgScreenRemoved: {
        QString screenName;
        in >> screenName;
        Q_EMIT screenRemoved(screenName);
        break;
    }
    default:
        sonicWarning() << "GreeterIpcClient: Unknown message type" << messageType;
        break;
    }
}

void GreeterIpcClient::send(quint32 messageType, const QByteArray &block)
{
    if (!m_connection || !m_connection->send(messageType, block)) {
        sonicWarning() << "GreeterIpcClient: Not connected, cannot send message" << messageType;
    }
}

void GreeterIpcClient::registerWindow(uint winId, const QString &screenName)
{
    if (!m_connection) {
        sonicWarning() << "GreeterIpcClient: Not connected, cannot register window";
        return;
    }

    quint32 messageId = m_nextMessageId++;

    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out << MsgRegisterWindow << messageId << static_cast<quint32>(winId) << screenName;

    send(MsgRegisterWindow, block);
}

void GreeterIpcClient::unregisterWindow(uint winId)
{
    if (!m_connection) {
        sonicWarning() << "GreeterIpcClient: Not connected, cannot unregister window";
        return;
    }

    quint32 messageId = m_nextMessageId++;

    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out << MsgUnregisterWindow << messageId << static_cast<quint32>(winId);

    send(MsgUnregisterWindow, block);
}

void GreeterIpcClient::reportAuthenticationSuccess()
{
    if (!m_connection) {
        sonicWarning() << "GreeterIpcClient: Not connected, cannot report auth success";
        return;
    }

    quint32 messageId = m_nextMessageId++;

    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out << MsgAuthenticationSuccess << messageId;

    send(MsgAuthenticationSuccess, block);
}

void GreeterIpcClient::requestFocus(const QString &screenName)
{
    if (!m_connection) {
        sonicWarning() << "GreeterIpcClient: Not connected, cannot request focus";
        return;
    }

    quint32 messageId = m_nextMessageId++;

    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out << MsgGetFocus << messageId << screenName;

    send(MsgGetFocus, block);
}
