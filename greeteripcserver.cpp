/*
    SPDX-FileCopyrightText: 2024 KDE contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "greeteripcserver.h"

#include <Sonic/Logging/Log>

#include <QCoreApplication>
#include <QDataStream>
#include <QIODevice>

#include <Sonic/Ipc/Connection>
#include <Sonic/Ipc/Server>

GreeterIpcServer::GreeterIpcServer(QObject *parent)
    : QObject(parent)
    , m_server(nullptr)
{
}

GreeterIpcServer::~GreeterIpcServer()
{
    // Sonic::Ipc::Server aborts its connections before closing.
    delete m_server;
    m_server = nullptr;
}

QString GreeterIpcServer::socketName()
{
    // Use a unique socket name based on the process ID
    return QStringLiteral("kscreenlocker-greeter-ipc-%1").arg(QCoreApplication::applicationPid());
}

bool GreeterIpcServer::initialize()
{
    if (m_server) {
        sonicWarning() << "GreeterIpcServer: Already initialized";
        return true;
    }

    m_server = new Sonic::Ipc::Server(this);

    // A stale socket with the same name is replaced; a live one never is.
    if (!m_server->listen(socketName(), Sonic::Ipc::Server::Access::User, greeterIpcProtocolVersion)) {
        sonicWarning() << "GreeterIpcServer: Failed to listen on socket" << socketName() << "Error:" << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }

    connect(m_server, &Sonic::Ipc::Server::newConnection, this, &GreeterIpcServer::handleNewConnection);

    return true;
}

void GreeterIpcServer::handleNewConnection(Sonic::Ipc::Connection *connection)
{
    connect(connection, &Sonic::Ipc::Connection::frameReceived, this, [this, connection](quint32 frameType, const QByteArray &payload) {
        processFrame(connection, frameType, payload);
    });
}

void GreeterIpcServer::processFrame(Sonic::Ipc::Connection *connection, quint32 frameType, const QByteArray &payload)
{
    QDataStream in(payload);

    // Read message header: messageType + messageId
    quint32 messageType;
    quint32 messageId;
    in >> messageType >> messageId;
    if (messageType != frameType) {
        sonicWarning() << "GreeterIpcServer: frame type" << frameType << "doesn't match message type" << messageType;
        sendResponse(connection, messageId, false);
        return;
    }

    switch (messageType) {
    case MsgRegisterWindow: {
        quint32 winId;
        QString screenName;
        in >> winId >> screenName;
        Q_EMIT greeterWindowRegistered(winId, screenName);
        sendResponse(connection, messageId, true);
        break;
    }
    case MsgUnregisterWindow: {
        quint32 winId;
        in >> winId;
        Q_EMIT greeterWindowUnregistered(winId);
        sendResponse(connection, messageId, true);
        break;
    }
    case MsgAuthenticationSuccess: {
        Q_EMIT greeterAuthenticationSuccess();
        sendResponse(connection, messageId, true);
        break;
    }
    case MsgGetFocus: {
        QString screenName;
        in >> screenName;
        Q_EMIT greeterGetFocusRequested(screenName);
        sendResponse(connection, messageId, true);
        break;
    }
    case MsgScreenAdded: {
        QString screenName;
        QRect geometry;
        in >> screenName >> geometry;
        Q_EMIT screenAdded(screenName, geometry);
        sendResponse(connection, messageId, true);
        break;
    }
    case MsgScreenRemoved: {
        QString screenName;
        in >> screenName;
        Q_EMIT screenRemoved(screenName);
        sendResponse(connection, messageId, true);
        break;
    }
    default:
        sonicWarning() << "GreeterIpcServer: Unknown message type" << messageType;
        sendResponse(connection, messageId, false);
        break;
    }
}

void GreeterIpcServer::sendResponse(Sonic::Ipc::Connection *connection, quint32 messageId, bool success)
{
    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out << MsgResponse << messageId << static_cast<quint8>(success ? 1 : 0);

    connection->send(MsgResponse, block);
}
