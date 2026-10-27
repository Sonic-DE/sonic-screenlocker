/*
    SPDX-FileCopyrightText: 2024 KDE contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QHash>
#include <QObject>
#include <QRect>
#include <QString>

namespace Sonic::Ipc {
class Connection;
class Server;
}

//! Sonic IPC protocol version of the greeter socket; the greeter's GreeterIpcClient ships with it.
constexpr quint16 greeterIpcProtocolVersion = 1;

/**
 * @class GreeterIpcServer
 * @brief Custom IPC server for greeter communication over Sonic IPC frames.
 *
 * This class replaces both the D-Bus GreeterAdaptor and Qt Remote Objects
 * for communication between the greeter process and KSldApp. Each message
 * is one frame whose type is the message type; the payload keeps the
 * QDataStream fields [type][id][arguments...].
 *
 * The server listens on a well-known socket name and handles messages
 * from connected greeter clients.
 */
class GreeterIpcServer : public QObject
{
    Q_OBJECT

public:
    /**
     * Constructs the GreeterIpcServer.
     * @param parent The parent QObject.
     */
    explicit GreeterIpcServer(QObject *parent = nullptr);

    /**
     * Destructor. Closes the server and disconnects all clients.
     */
    ~GreeterIpcServer() override;

    /**
     * Initializes the IPC server and starts listening for connections.
     * @return true if the server was successfully started, false otherwise.
     */
    bool initialize();

    /**
     * Returns the socket name used for IPC communication.
     * @return The socket name.
     */
    static QString socketName();

Q_SIGNALS:
    /**
     * Emitted when a greeter window is registered.
     * @param winId The X11 window ID of the greeter view.
     * @param screenName The name of the screen the view is on.
     */
    void greeterWindowRegistered(uint winId, const QString &screenName);

    /**
     * Emitted when a greeter window is unregistered.
     * @param winId The X11 window ID of the greeter view.
     */
    void greeterWindowUnregistered(uint winId);

    /**
     * Emitted when the greeter reports authentication success.
     */
    void greeterAuthenticationSuccess();

    /**
     * Emitted when the greeter requests focus on a screen.
     * @param screenName The name of the screen to focus.
     */
    void greeterGetFocusRequested(const QString &screenName);

    /**
     * Emitted when a new screen is added.
     * @param screenName The name of the added screen.
     * @param geometry The geometry of the added screen.
     */
    void screenAdded(const QString &screenName, const QRect &geometry);

    /**
     * Emitted when a screen is removed.
     * @param screenName The name of the removed screen.
     */
    void screenRemoved(const QString &screenName);

private Q_SLOTS:
    /**
     * Handles a new greeter connection.
     * @param connection The new connection.
     */
    void handleNewConnection(Sonic::Ipc::Connection *connection);

private:
    /**
     * Processes one frame from a client.
     * @param connection The client connection.
     * @param frameType The frame type, which is the message type.
     * @param payload The message's stream bytes.
     */
    void processFrame(Sonic::Ipc::Connection *connection, quint32 frameType, const QByteArray &payload);

    /**
     * Sends a response to a client.
     * @param connection The client connection.
     * @param messageId The id of the message answered.
     * @param success Whether the operation was successful.
     */
    void sendResponse(Sonic::Ipc::Connection *connection, quint32 messageId, bool success);

    /**
     * Message types for IPC communication.
     */
    enum MessageType {
        MsgRegisterWindow = 1,
        MsgUnregisterWindow = 2,
        MsgAuthenticationSuccess = 3,
        MsgGetFocus = 4,
        MsgScreenAdded = 5,
        MsgScreenRemoved = 6,
        MsgResponse = 100,
    };

    Sonic::Ipc::Server *m_server = nullptr;
};
