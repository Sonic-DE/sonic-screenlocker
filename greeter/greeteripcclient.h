/*
    SPDX-FileCopyrightText: 2024 KDE contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QRect>
#include <QString>
#include <functional>

namespace Sonic::Ipc {
class Client;
class Connection;
}

/**
 * @class GreeterIpcClient
 * @brief Custom IPC client for greeter communication over Sonic IPC frames.
 *
 * This class provides the client-side communication with the GreeterIpcServer
 * running in KSldApp. Each message is one frame whose type is the message
 * type; the payload keeps the QDataStream fields [type][id][arguments...].
 *
 * The greeter uses this client to:
 * - Register/unregister greeter windows
 * - Report authentication success
 * - Request focus on screens
 * - Receive screen added/removed notifications
 */
class GreeterIpcClient : public QObject
{
    Q_OBJECT

public:
    /**
     * Constructs the GreeterIpcClient.
     * @param parent The parent QObject.
     */
    explicit GreeterIpcClient(QObject *parent = nullptr);

    /**
     * Destructor. Closes the connection.
     */
    ~GreeterIpcClient() override;

    /**
     * Returns the socket name used for IPC communication.
     * @return The socket name.
     */
    static QString socketName();

    /**
     * Connects to the GreeterIpcServer.
     * @return true if connection was initiated, false otherwise.
     */
    bool connectToHost();

    /**
     * Disconnects from the server.
     */
    void disconnectFromHost();

    /**
     * Returns whether the client is connected.
     * @return true if connected, false otherwise.
     */
    bool isConnected() const;

Q_SIGNALS:
    /**
     * Emitted when the connection to the server is established.
     */
    void connected();

    /**
     * Emitted when the connection to the server is lost.
     */
    void disconnected();

    /**
     * Emitted when a connection error occurs.
     * @param errorString Description of the error.
     */
    void connectionError(const QString &errorString);

    /**
     * Emitted when a screen is added.
     * @param screenName The name of the added screen.
     * @param geometry The geometry of the added screen.
     */
    void screenAdded(const QString &screenName, const QRect &geometry);

    /**
     * Emitted when a screen is removed.
     * @param screenName The name of the removed screen.
     */
    void screenRemoved(const QString &screenName);

public Q_SLOTS:
    /**
     * Registers a greeter window with KSldApp.
     * @param winId The X11 window ID of the greeter view.
     * @param screenName The name of the screen the view is on.
     */
    void registerWindow(uint winId, const QString &screenName);

    /**
     * Unregisters a greeter window from KSldApp.
     * @param winId The X11 window ID of the greeter view.
     */
    void unregisterWindow(uint winId);

    /**
     * Reports authentication success to KSldApp.
     */
    void reportAuthenticationSuccess();

    /**
     * Requests focus on a screen.
     * @param screenName The name of the screen to focus.
     */
    void requestFocus(const QString &screenName);

private:
    void handleConnected(Sonic::Ipc::Connection *connection);
    void handleFrame(quint32 frameType, const QByteArray &payload);

    /**
     * Sends one message to the server as one frame.
     * @param messageType The type of message.
     * @param block The message's stream bytes, starting with [type][id].
     */
    void send(quint32 messageType, const QByteArray &block);

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

    Sonic::Ipc::Client *m_client = nullptr;
    Sonic::Ipc::Connection *m_connection = nullptr;
    quint32 m_nextMessageId = 1;
    QHash<quint32, std::function<void(bool)>> m_pendingCallbacks;
};
