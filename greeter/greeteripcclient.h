/*
    SPDX-FileCopyrightText: 2024 KDE contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QDataStream>
#include <QHash>
#include <QLocalSocket>
#include <QObject>
#include <QRect>
#include <QString>
#include <functional>

/**
 * @class GreeterIpcClient
 * @brief Custom IPC client for greeter communication using QLocalSocket.
 *
 * This class provides the client-side communication with the GreeterIpcServer
 * running in KSldApp. It uses QLocalSocket/QLocalServer with QDataStream
 * for reliable local IPC.
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

private Q_SLOTS:
    void handleConnected();
    void handleDisconnected();
    void handleError(QLocalSocket::LocalSocketError socketError);
    void handleDataAvailable();

private:
    /**
     * Sends a message to the server.
     * @param messageType The type of message.
     * @param data The message data.
     */
    void sendMessage(quint32 messageType, QDataStream &data);

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

    QLocalSocket *m_socket = nullptr;
    quint32 m_nextMessageId = 1;
    QHash<quint32, std::function<void(bool)>> m_pendingCallbacks;
};
