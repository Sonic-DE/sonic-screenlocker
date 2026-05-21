/*
    SPDX-FileCopyrightText: 2024 KDE contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "greeteripcserver.h"

#include "kscreenlocker_logging.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QLocalServer>
#include <QLocalSocket>
#include <QThread>

GreeterIpcServer::GreeterIpcServer(QObject *parent)
    : QObject(parent)
    , m_server(nullptr)
{
}

GreeterIpcServer::~GreeterIpcServer()
{
    if (m_server) {
        // Disconnect all pending connections
        for (QLocalSocket *client : m_server->findChildren<QLocalSocket *>()) {
            client->disconnect();
            client->close();
        }
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
}

QString GreeterIpcServer::socketName()
{
    // Use a unique socket name based on the process ID
    return QStringLiteral("kscreenlocker-greeter-ipc-%1").arg(QCoreApplication::applicationPid());
}

bool GreeterIpcServer::initialize()
{
    if (m_server) {
        qCWarning(KSCREENLOCKER) << "GreeterIpcServer: Already initialized";
        return true;
    }

    m_server = new QLocalServer(this);

    // Remove any existing socket with the same name
    QLocalServer::removeServer(socketName());

    if (!m_server->listen(socketName())) {
        qCWarning(KSCREENLOCKER) << "GreeterIpcServer: Failed to listen on socket" << socketName() << "Error:" << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }

    connect(m_server, &QLocalServer::newConnection, this, &GreeterIpcServer::handleNewConnection);

    return true;
}

void GreeterIpcServer::handleNewConnection()
{
    QLocalSocket *clientSocket = m_server->nextPendingConnection();
    if (!clientSocket) {
        return;
    }

    connect(clientSocket, &QLocalSocket::readyRead, this, [this, clientSocket]() {
        handleDataAvailable(clientSocket);
    });
    connect(clientSocket, &QLocalSocket::disconnected, this, [this, clientSocket]() {
        handleClientDisconnected(clientSocket);
    });
}

void GreeterIpcServer::handleDataAvailable(QLocalSocket *clientSocket)
{
    QDataStream in(clientSocket);

    // Read message header: messageType + messageId
    while (clientSocket->bytesAvailable() >= static_cast<qint64>(sizeof(quint64))) {
        quint32 messageType;
        quint32 messageId;
        in >> messageType >> messageId;

        switch (messageType) {
        case MsgRegisterWindow: {
            quint32 winId;
            QString screenName;
            in >> winId >> screenName;
            Q_EMIT greeterWindowRegistered(winId, screenName);
            sendResponse(clientSocket, messageId, true);
            break;
        }
        case MsgUnregisterWindow: {
            quint32 winId;
            in >> winId;
            Q_EMIT greeterWindowUnregistered(winId);
            sendResponse(clientSocket, messageId, true);
            break;
        }
        case MsgAuthenticationSuccess: {
            Q_EMIT greeterAuthenticationSuccess();
            sendResponse(clientSocket, messageId, true);
            break;
        }
        case MsgGetFocus: {
            QString screenName;
            in >> screenName;
            Q_EMIT greeterGetFocusRequested(screenName);
            sendResponse(clientSocket, messageId, true);
            break;
        }
        case MsgScreenAdded: {
            QString screenName;
            QRect geometry;
            in >> screenName >> geometry;
            Q_EMIT screenAdded(screenName, geometry);
            sendResponse(clientSocket, messageId, true);
            break;
        }
        case MsgScreenRemoved: {
            QString screenName;
            in >> screenName;
            Q_EMIT screenRemoved(screenName);
            sendResponse(clientSocket, messageId, true);
            break;
        }
        default:
            qCWarning(KSCREENLOCKER) << "GreeterIpcServer: Unknown message type" << messageType;
            sendResponse(clientSocket, messageId, false);
            break;
        }
    }
}

void GreeterIpcServer::handleClientDisconnected(QLocalSocket *clientSocket)
{
    m_clientMessageIds.remove(clientSocket);
    clientSocket->deleteLater();
}

void GreeterIpcServer::sendResponse(QLocalSocket *clientSocket, quint32 messageId, bool success)
{
    if (!clientSocket || clientSocket->state() != QLocalSocket::ConnectedState) {
        return;
    }

    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out << MsgResponse << messageId << static_cast<quint8>(success ? 1 : 0);

    clientSocket->write(block);
    clientSocket->flush();
}
