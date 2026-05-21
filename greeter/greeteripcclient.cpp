/*
    SPDX-FileCopyrightText: 2024 KDE contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "greeteripcclient.h"

#include "kscreenlocker_greet_logging.h"

GreeterIpcClient::GreeterIpcClient(QObject *parent)
    : QObject(parent)
    , m_socket(nullptr)
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
    if (m_socket) {
        if (m_socket->state() == QLocalSocket::ConnectedState) {
            return true;
        }
        delete m_socket;
    }

    m_socket = new QLocalSocket(this);

    connect(m_socket, &QLocalSocket::connected, this, &GreeterIpcClient::handleConnected);
    connect(m_socket, &QLocalSocket::disconnected, this, &GreeterIpcClient::handleDisconnected);
    connect(m_socket, &QLocalSocket::errorOccurred, this, &GreeterIpcClient::handleError);
    connect(m_socket, &QLocalSocket::readyRead, this, &GreeterIpcClient::handleDataAvailable);

    // Get socket name from environment variable set by KSldApp
    QString socketName = QString::fromUtf8(qgetenv("KSCREENLOCKER_IPC_SOCKET"));
    if (socketName.isEmpty()) {
        // Fallback to default
        socketName = GreeterIpcClient::socketName();
    }

    m_socket->connectToServer(socketName);

    return true;
}

void GreeterIpcClient::disconnectFromHost()
{
    if (m_socket) {
        m_socket->disconnect();
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

bool GreeterIpcClient::isConnected() const
{
    return m_socket && m_socket->state() == QLocalSocket::ConnectedState;
}

void GreeterIpcClient::handleConnected()
{
    Q_EMIT connected();
}

void GreeterIpcClient::handleDisconnected()
{
    Q_EMIT disconnected();
}

void GreeterIpcClient::handleError(QLocalSocket::LocalSocketError socketError)
{
    qCCritical(KSCREENLOCKER_GREET) << "GreeterIpcClient: Error" << socketError << m_socket->errorString();
    Q_EMIT connectionError(m_socket->errorString());
}

void GreeterIpcClient::handleDataAvailable()
{
    if (!m_socket) {
        return;
    }

    QDataStream in(m_socket);

    while (m_socket->bytesAvailable() >= sizeof(quint32) * 2) {
        quint32 messageType;
        quint32 messageId;
        in >> messageType >> messageId;

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
            qCWarning(KSCREENLOCKER_GREET) << "GreeterIpcClient: Unknown message type" << messageType;
            break;
        }
    }
}

void GreeterIpcClient::sendMessage(quint32 messageType, QDataStream &data)
{
    if (!m_socket || m_socket->state() != QLocalSocket::ConnectedState) {
        qCWarning(KSCREENLOCKER_GREET) << "GreeterIpcClient: Not connected, cannot send message" << messageType;
        return;
    }

    quint32 messageId = m_nextMessageId++;

    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out << messageType << messageId;

    // Append the data from the passed stream
    // We need to get the data that was serialized
    // Since we can't easily do that, we'll refactor later

    m_socket->write(block);
    m_socket->flush();
}

void GreeterIpcClient::registerWindow(uint winId, const QString &screenName)
{
    if (!m_socket || m_socket->state() != QLocalSocket::ConnectedState) {
        qCWarning(KSCREENLOCKER_GREET) << "GreeterIpcClient: Not connected, cannot register window";
        return;
    }

    quint32 messageId = m_nextMessageId++;

    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out << MsgRegisterWindow << messageId << static_cast<quint32>(winId) << screenName;

    m_socket->write(block);
    m_socket->flush();
}

void GreeterIpcClient::unregisterWindow(uint winId)
{
    if (!m_socket || m_socket->state() != QLocalSocket::ConnectedState) {
        qCWarning(KSCREENLOCKER_GREET) << "GreeterIpcClient: Not connected, cannot unregister window";
        return;
    }

    quint32 messageId = m_nextMessageId++;

    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out << MsgUnregisterWindow << messageId << static_cast<quint32>(winId);

    m_socket->write(block);
    m_socket->flush();
}

void GreeterIpcClient::reportAuthenticationSuccess()
{
    if (!m_socket || m_socket->state() != QLocalSocket::ConnectedState) {
        qCWarning(KSCREENLOCKER_GREET) << "GreeterIpcClient: Not connected, cannot report auth success";
        return;
    }

    quint32 messageId = m_nextMessageId++;

    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out << MsgAuthenticationSuccess << messageId;

    m_socket->write(block);
    m_socket->flush();
}

void GreeterIpcClient::requestFocus(const QString &screenName)
{
    if (!m_socket || m_socket->state() != QLocalSocket::ConnectedState) {
        qCWarning(KSCREENLOCKER_GREET) << "GreeterIpcClient: Not connected, cannot request focus";
        return;
    }

    quint32 messageId = m_nextMessageId++;

    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out << MsgGetFocus << messageId << screenName;

    m_socket->write(block);
    m_socket->flush();
}
