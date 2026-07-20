// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// SPDX-FileCopyrightText: 2020 David Edmundson <davidedmundson@kde.org>
// SPDX-FileCopyrightText: 2026 Harald Sitter <sitter@kde.org>

#include "pamworker.h"

#include <QDBusPendingCallWatcher>
#include <QDBusServer>
#include <QProcess>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <utility>

#include <KLibexec>

#include "kscreenlocker_greet_logging.h"
#include "result.h"
#include "screenlockeradaptor.h"

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

PamWorker::PamWorker(const QString &service, const QString &user)
    : QObject(nullptr)
    , m_nextAttemptAllowedTime(std::chrono::steady_clock::now())
    , m_service(service)
    , m_username(user)
{
}

PamWorker::~PamWorker()
{
    quitWorkerProcess();
}

void PamWorker::authenticate(quint64 requestId)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (m_unavailable) {
        return;
    }
    if (!m_workerProcess) {
        start();
    }
    if (!m_initialized || m_inAuthenticate || std::chrono::steady_clock::now() < m_nextAttemptAllowedTime) {
        m_deferredRequestId = requestId;
        if (std::chrono::steady_clock::now() < m_nextAttemptAllowedTime) {
            scheduleDelayTimer();
        }
        return;
    }

    m_currentRequestId = requestId;
    m_inAuthenticate = true;
    Q_EMIT inAuthenticateChanged(true);
    Q_EMIT busyChanged(true);
    const quint64 generation = m_generation;
    auto watcher = new QDBusPendingCallWatcher(m_dbusWorker->Authenticate(), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, requestId, generation] {
        watcher->deleteLater();
        // Cancellation tears down the process. Its late D-Bus reply must never unlock a new session.
        if (generation != m_generation || requestId != m_currentRequestId) {
            return;
        }
        m_inAuthenticate = false;
        m_currentRequestId = 0;
        Q_EMIT inAuthenticateChanged(false);
        Q_EMIT busyChanged(false);
        if (watcher->error().isValid()) {
            qCWarning(KSCREENLOCKER_GREET) << "PAM worker failed to authenticate:" << watcher->error().message();
            setUnavailable();
            Q_EMIT failed(requestId);
            return;
        }
        decltype(m_dbusWorker->Authenticate()) reply = watcher->reply();
        switch (reply.value()) {
        case WorkerResult::Type::Failure:
            Q_EMIT failed(requestId);
            break;
        case WorkerResult::Type::Success:
            m_deferredRequestId = 0;
            Q_EMIT succeeded(requestId);
            return;
        case WorkerResult::Type::Unavailable:
            setUnavailable();
            return;
        default:
            setUnavailable();
            return;
        }
        startDeferredAuthentication();
    });
}

void PamWorker::StartFailedDelay(uint useconds)
{
    if (useconds == 0) {
        return;
    }
    m_nextAttemptAllowedTime = std::max(m_nextAttemptAllowedTime, std::chrono::steady_clock::now() + std::chrono::microseconds(useconds));
    if (!m_inPasswordDelay) {
        m_inPasswordDelay = true;
        Q_EMIT inPasswordDelayChanged(true);
    }
    scheduleDelayTimer();
    Q_EMIT loginFailedDelayStarted(useconds);
}

void PamWorker::scheduleDelayTimer()
{
    if (!m_delayTimer) {
        m_delayTimer = new QTimer(this);
        m_delayTimer->setSingleShot(true);
        m_delayTimer->setTimerType(Qt::PreciseTimer);
        connect(m_delayTimer, &QTimer::timeout, this, &PamWorker::scheduleDelayTimer);
    }
    const auto remaining = m_nextAttemptAllowedTime - std::chrono::steady_clock::now();
    if (remaining > std::chrono::steady_clock::duration::zero()) {
        m_delayTimer->start(std::chrono::ceil<std::chrono::milliseconds>(remaining));
        return;
    }
    if (m_inPasswordDelay) {
        m_inPasswordDelay = false;
        Q_EMIT inPasswordDelayChanged(false);
    }
    startDeferredAuthentication();
}

void PamWorker::startDeferredAuthentication()
{
    if (!m_initialized || m_inAuthenticate || m_unavailable || m_deferredRequestId == 0 || std::chrono::steady_clock::now() < m_nextAttemptAllowedTime) {
        return;
    }
    authenticate(std::exchange(m_deferredRequestId, 0));
}

void PamWorker::respond(quint64 requestId, const QByteArray &response)
{
    if (requestId != m_currentRequestId || !m_dbusWorker || m_pendingPrompt.type() == QDBusMessage::InvalidMessage) {
        return;
    }
    m_dbusWorker->connection().send(m_pendingPrompt.createReply(QString::fromUtf8(response)));
    m_pendingPrompt = {};
    Q_EMIT busyChanged(true);
}

void PamWorker::cancel(quint64 requestId)
{
    if (m_deferredRequestId == requestId) {
        m_deferredRequestId = 0;
    }
    if (m_currentRequestId != 0 && m_currentRequestId != requestId) {
        return;
    }
    quitWorkerProcess();
    Q_EMIT authenticationCancelled(requestId);
}

void PamWorker::InfoMessage(const QString &msg)
{
    Q_EMIT infoMessage(msg);
}

void PamWorker::ErrorMessage(const QString &msg)
{
    Q_EMIT errorMessage(msg);
}

void PamWorker::setUnavailable()
{
    if (!m_unavailable) {
        m_unavailable = true;
        m_deferredRequestId = 0;
        Q_EMIT unavailabilityChanged(true);
    }
}

void PamWorker::start()
{
    if (m_workerProcess || m_unavailable) {
        return;
    }
    if (!findChild<ScreenlockerAdaptor *>()) {
        new ScreenlockerAdaptor(this);
    }
    m_server = new QDBusServer(this);
    if (!m_server->isConnected()) {
        setUnavailable();
        return;
    }
    connect(m_server, &QDBusServer::newConnection, this, [this](const QDBusConnection &connection) {
        if (m_dbusWorker) {
            // A runtime check, not just an assertion: accept exactly one peer in release builds too.
            QDBusConnection::disconnectFromPeer(connection.name());
            return;
        }
        auto c = connection;
        if (!c.registerObject(u"/org/kde/plasma/screenlocker"_s, this, QDBusConnection::ExportAdaptors)) {
            setUnavailable();
            return;
        }
        m_dbusWorker = std::make_unique<org::kde::plasma::screenlocker::worker>(QString(), u"/org/kde/plasma/screenlocker/worker"_s, c);
        m_dbusWorker->setTimeout(std::numeric_limits<int>::max());
    });

    m_workerProcess = new QProcess(this);
    m_workerProcess->setProcessChannelMode(QProcess::ForwardedChannels);
    m_workerProcess->setProgram(KLibexec::path(u"kscreenlocker_worker"_s));
    m_workerProcess->setArguments({m_service});
    connect(m_workerProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        setUnavailable();
    });
    connect(m_workerProcess, &QProcess::finished, this, [this] {
        setUnavailable();
    });
    m_workerProcess->start();
    m_workerProcess->write(m_server->address().toUtf8());
    m_workerProcess->closeWriteChannel();
}

void PamWorker::Ping(const QString &)
{
    if (!m_dbusWorker || connection().name() != m_dbusWorker->connection().name() || m_initialized) {
        return;
    }
    const quint64 generation = m_generation;
    auto watcher = new QDBusPendingCallWatcher(m_dbusWorker->Start(m_service, m_username), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_generation) {
            return;
        }
        if (watcher->error().isValid()) {
            setUnavailable();
            return;
        }
        m_initialized = true;
        startDeferredAuthentication();
    });
}

QString PamWorker::Prompt(const QString &msg)
{
    return promptInternal(msg, false);
}

QString PamWorker::MaskedPrompt(const QString &msg)
{
    return promptInternal(msg, true);
}

QString PamWorker::promptInternal(const QString &msg, bool isSecret)
{
    if (!m_inAuthenticate || m_pendingPrompt.type() != QDBusMessage::InvalidMessage) {
        return {};
    }
    // Record the delayed call before publishing the prompt, so an immediate response cannot be lost.
    setDelayedReply(true);
    m_pendingPrompt = message();
    Q_EMIT busyChanged(false);
    if (isSecret) {
        Q_EMIT promptForSecret(m_currentRequestId, msg);
    } else {
        Q_EMIT prompt(m_currentRequestId, msg);
    }
    return {};
}

void PamWorker::quitWorkerProcess()
{
    ++m_generation;
    m_initialized = false;
    m_pendingPrompt = {};
    m_currentRequestId = 0;
    m_inAuthenticate = false;
    Q_EMIT inAuthenticateChanged(false);
    Q_EMIT busyChanged(false);
    if (m_workerProcess) {
        m_workerProcess->disconnect(this);
        m_workerProcess->terminate();
        if (!m_workerProcess->waitForFinished((25ms).count())) {
            m_workerProcess->kill();
            m_workerProcess->waitForFinished();
        }
        delete std::exchange(m_workerProcess, nullptr);
    }
    if (m_dbusWorker) {
        const auto name = m_dbusWorker->connection().name();
        m_dbusWorker.reset();
        QDBusConnection::disconnectFromPeer(name);
    }
    delete std::exchange(m_server, nullptr);
}
