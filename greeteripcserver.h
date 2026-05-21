/*
    SPDX-FileCopyrightText: 2024 KDE contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QHash>
#include <QObject>
#include <QRect>
#include <QString>

class QLocalServer;
class QLocalSocket;

/**
 * @class GreeterIpcServer
 * @brief Custom IPC server for greeter communication using QLocalSocket/QLocalServer.
 *
 * This class replaces both the D-Bus GreeterAdaptor and Qt Remote Objects
 * for communication between the greeter process and KSldApp. It uses
 * QLocalServer/QLocalSocket with QDataStream for reliable local IPC.
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
     * Handles new incoming connections.
     */
    void handleNewConnection();

    /**
     * Handles data available on a client socket.
     * @param clientSocket The client socket with available data.
     */
    void handleDataAvailable(QLocalSocket *clientSocket);

    /**
     * Handles client disconnection.
     * @param clientSocket The disconnected client socket.
     */
    void handleClientDisconnected(QLocalSocket *clientSocket);

private:
    /**
     * Processes an incoming message from a client.
     * @param clientSocket The client socket.
     * @param messageType The type of message.
     * @param data The message data.
     */
    void processMessage(QLocalSocket *clientSocket, quint32 messageType, QDataStream &data);

    /**
     * Sends a response to a client.
     * @param clientSocket The client socket.
     * @param messageType The type of message ( echoed back).
     * @param success Whether the operation was successful.
     */
    void sendResponse(QLocalSocket *clientSocket, quint32 messageType, bool success);

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

    QLocalServer *m_server = nullptr;
    QHash<QLocalSocket *, quint32> m_clientMessageIds;
};
