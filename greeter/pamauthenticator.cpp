/*
    SPDX-FileCopyrightText: 2020 David Edmundson <davidedmundson@kde.org>
    SPDX-FileCopyrightText: 2026 Harald Sitter <sitter@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pamauthenticator.h"

#include <QDBusPendingCallWatcher>
#include <QDBusServer>
#include <QProcess>
#include <QTimer>
#include <algorithm>
#include <utility>

#include <KLibexec>

#include "org.kde.plasma.screenlocker.worker.h"
#include "result.h"
#include "screenlockeradaptor.h"
#include <Sonic/Logging/Log>

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

PamAuthenticator::PamAuthenticator(const QString &service, const QString &user, NoninteractiveAuthenticatorTypes types, QObject *parent)
    : QObject(parent)
    , m_service(service)
    , m_authenticatorType(types)
    , m_user(user)
{
    new ScreenlockerAdaptor(this);
    startWorker();
}

PamAuthenticator::~PamAuthenticator()
{
    m_cancelling = true;
    quitWorkerProcess();
}

bool PamAuthenticator::isBusy() const
{
    return m_busy;
}

bool PamAuthenticator::isReady() const
{
    return m_ready;
}

bool PamAuthenticator::isAvailable() const
{
    return !m_unavailable;
}

bool PamAuthenticator::isUnavailable() const
{
    return m_unavailable;
}

bool PamAuthenticator::inPasswordDelay() const
{
    return m_inPasswordDelay;
}

bool PamAuthenticator::isPromptReady() const
{
    return m_promptReady;
}

quint64 PamAuthenticator::currentRequestId() const
{
    return m_requestId;
}

PamAuthenticator::NoninteractiveAuthenticatorTypes PamAuthenticator::authenticatorType() const
{
    return m_authenticatorType;
}

void PamAuthenticator::setBusy(bool busy)
{
    if (m_busy != busy) {
        m_busy = busy;
        Q_EMIT busyChanged();
    }
}

void PamAuthenticator::setReady(bool ready)
{
    if (m_ready != ready) {
        m_ready = ready;
        Q_EMIT readyChanged();
    }
}

bool PamAuthenticator::isUnlocked() const
{
    return m_unlocked;
}

void PamAuthenticator::tryUnlock()
{
    if (m_unlocked || m_unavailable || m_authenticationRequested || m_cancelling) {
        return;
    }
    m_authenticationRequested = true;
    m_hadPromptForRequest = false;
    clearPromptState();
    ++m_requestId;
    if (!m_workerProcess) {
        startWorker();
    }
    if (std::chrono::steady_clock::now() < m_nextAttemptAllowedTime) {
        scheduleDelayTimer();
    }
    startDeferredAuthentication();
}

void PamAuthenticator::startDeferredAuthentication()
{
    // Keep early requests queued, but never send Authenticate until the worker handshake completes.
    if (!m_ready || !m_initialized || !m_dbusWorker || !m_authenticationRequested || m_inAuthenticate || m_unavailable
        || std::chrono::steady_clock::now() < m_nextAttemptAllowedTime) {
        return;
    }
    m_inAuthenticate = true;
    const auto requestId = m_requestId;
    const auto generation = m_generation;
    setBusy(true);
    if (generation != m_generation || requestId != m_requestId || !m_inAuthenticate) {
        return;
    }
    auto watcher = new QDBusPendingCallWatcher(m_dbusWorker->Authenticate(), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, requestId, generation] {
        watcher->deleteLater();
        // Cancellation or a worker restart invalidates even replies already queued for delivery.
        if (generation != m_generation || requestId != m_requestId || !m_inAuthenticate) {
            return;
        }
        m_inAuthenticate = false;
        clearPromptState();
        if (generation != m_generation || requestId != m_requestId) {
            return;
        }
        m_authenticationRequested = false;
        if (watcher->error().isValid()) {
            sonicWarning() << "PAM worker failed to authenticate:" << watcher->error().message();
            setUnavailable();
            Q_EMIT failed();
            return;
        }
        decltype(m_dbusWorker->Authenticate()) reply = watcher->reply();
        switch (reply.value()) {
        case WorkerResult::Failure:
            handleFailedAuthentication();
            return;
        case WorkerResult::Success:
            m_unlocked = true;
            setBusy(false);
            Q_EMIT succeeded();
            return;
        case WorkerResult::Unavailable:
            setUnavailable();
            return;
        default:
            sonicWarning() << "Unexpected authentication result" << reply.value();
            setUnavailable();
            return;
        }
    });
}

void PamAuthenticator::handleFailedAuthentication()
{
    // Broken promptless services must not loop indefinitely, but wrong passwords must remain retryable.
    if (m_hadPromptForRequest) {
        m_failedCount = 0;
    } else {
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastFailed < 2s) {
            ++m_failedCount;
        } else {
            m_failedCount = 1;
        }
        m_lastFailed = now;
        if (m_failedCount >= 4) {
            setUnavailable();
        }
    }
    setBusy(false);
    Q_EMIT failed();
}

void PamAuthenticator::respond(const QByteArray &response)
{
    if (!m_promptReady || !m_inAuthenticate || !m_dbusWorker || m_pendingPrompt.type() == QDBusMessage::InvalidMessage) {
        sonicDebug() << "Ignoring response without an active PAM prompt";
        return;
    }
    // Consume the pending call before emitting signals; a second submission cannot reuse it.
    const auto prompt = std::exchange(m_pendingPrompt, {});
    m_promptReady = false;
    m_dbusWorker->connection().send(prompt.createReply(QString::fromUtf8(response)));
    setBusy(true);
    Q_EMIT promptReadyChanged();
}

void PamAuthenticator::cancel()
{
    if (m_cancelling) {
        return;
    }
    m_cancelling = true;
    m_authenticationRequested = false;
    ++m_requestId;
    clearPromptState();
    quitWorkerProcess();
    Q_EMIT authenticationCancelled();
    m_cancelling = false;
}

void PamAuthenticator::setInPasswordDelay(bool inPasswordDelay)
{
    if (m_inPasswordDelay != inPasswordDelay) {
        m_inPasswordDelay = inPasswordDelay;
        Q_EMIT inPasswordDelayChanged();
    }
}

void PamAuthenticator::clearPromptState()
{
    const bool wasReady = m_promptReady;
    m_promptReady = false;
    m_prompt.clear();
    m_promptForSecret.clear();
    m_infoMessage.clear();
    m_errorMessage.clear();
    if (wasReady) {
        Q_EMIT promptReadyChanged();
    }
}

QString PamAuthenticator::getPrompt() const
{
    return m_prompt;
}

QString PamAuthenticator::getPromptForSecret() const
{
    return m_promptForSecret;
}

QString PamAuthenticator::getInfoMessage() const
{
    return m_infoMessage;
}

QString PamAuthenticator::getErrorMessage() const
{
    return m_errorMessage;
}

QString PamAuthenticator::service() const
{
    return m_service;
}

void PamAuthenticator::Ping(const QString &)
{
    if (!calledFromDBus() || !m_dbusWorker || connection().name() != m_dbusWorker->connection().name() || m_initialized || m_unavailable || m_cancelling) {
        return;
    }
    // The worker initializes PAM and registers its object before sending Ping.
    m_initialized = true;
    setReady(true);
    startDeferredAuthentication();
}

QString PamAuthenticator::Prompt(const QString &msg)
{
    return promptInternal(msg, false);
}

QString PamAuthenticator::MaskedPrompt(const QString &msg)
{
    return promptInternal(msg, true);
}

void PamAuthenticator::StartFailedDelay(uint useconds)
{
    if (useconds == 0) {
        return;
    }
    m_nextAttemptAllowedTime = std::max(m_nextAttemptAllowedTime, std::chrono::steady_clock::now() + std::chrono::microseconds(useconds));
    setInPasswordDelay(true);
    scheduleDelayTimer();
    Q_EMIT loginFailedDelayStarted(useconds);
}

void PamAuthenticator::scheduleDelayTimer()
{
    if (!m_delayTimer) {
        m_delayTimer = new QTimer(this);
        m_delayTimer->setSingleShot(true);
        m_delayTimer->setTimerType(Qt::PreciseTimer);
        connect(m_delayTimer, &QTimer::timeout, this, &PamAuthenticator::scheduleDelayTimer);
    }
    const auto remaining = m_nextAttemptAllowedTime - std::chrono::steady_clock::now();
    if (remaining > std::chrono::steady_clock::duration::zero()) {
        m_delayTimer->start(std::chrono::ceil<std::chrono::milliseconds>(remaining));
        return;
    }
    setInPasswordDelay(false);
    startDeferredAuthentication();
}

void PamAuthenticator::InfoMessage(const QString &msg)
{
    m_infoMessage = msg;
    Q_EMIT infoMessage(msg);
}

void PamAuthenticator::ErrorMessage(const QString &msg)
{
    m_errorMessage = msg;
    Q_EMIT errorMessage(msg);
}

QString PamAuthenticator::promptInternal(const QString &msg, bool isSecret)
{
    if (!calledFromDBus() || !m_dbusWorker || connection().name() != m_dbusWorker->connection().name() || !m_inAuthenticate
        || m_pendingPrompt.type() != QDBusMessage::InvalidMessage) {
        return {};
    }
    // A queued response can now be flushed synchronously: record the call before publishing readiness.
    setDelayedReply(true);
    m_pendingPrompt = message();
    m_hadPromptForRequest = true;
    if (isSecret) {
        m_promptForSecret = msg;
    } else {
        m_prompt = msg;
    }
    m_promptReady = true;
    const auto generation = m_generation;
    const auto requestId = m_requestId;
    setBusy(false);
    if (generation != m_generation || requestId != m_requestId || !m_inAuthenticate) {
        return {};
    }
    Q_EMIT promptReadyChanged();
    if (generation != m_generation || requestId != m_requestId || !m_inAuthenticate) {
        return {};
    }
    if (isSecret) {
        Q_EMIT promptForSecret(msg);
    } else {
        Q_EMIT prompt(msg);
    }
    return {};
}

void PamAuthenticator::setUnavailable()
{
    if (m_unavailable) {
        return;
    }
    m_unavailable = true;
    m_authenticationRequested = false;
    m_inAuthenticate = false;
    m_initialized = false;
    clearPromptState();
    setReady(false);
    setBusy(false);
    Q_EMIT availableChanged();
}

void PamAuthenticator::quitWorkerProcess()
{
    ++m_generation;
    m_initialized = false;
    m_inAuthenticate = false;
    m_pendingPrompt = {};
    setReady(false);
    setBusy(false);
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

void PamAuthenticator::startWorker()
{
    if (m_workerProcess || m_unavailable) {
        return;
    }
    m_server = new QDBusServer(this);
    if (!m_server->isConnected()) {
        setUnavailable();
        return;
    }
    const auto generation = m_generation;
    connect(m_server, &QDBusServer::newConnection, this, [this, generation](const QDBusConnection &connection) {
        if (generation != m_generation) {
            QDBusConnection::disconnectFromPeer(connection.name());
            return;
        }
        connectWorker(connection);
    });
    m_workerProcess = new QProcess(this);
    m_workerProcess->setProcessChannelMode(QProcess::ForwardedChannels);
    m_workerProcess->setProgram(KLibexec::path(u"kscreenlocker_worker"_s));
    m_workerProcess->setArguments({m_service, m_user});
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

void PamAuthenticator::connectWorker(const QDBusConnection &connection)
{
    if (m_dbusWorker) {
        QDBusConnection::disconnectFromPeer(connection.name());
        return;
    }
    auto c = connection;
    if (!c.registerObject(u"/org/kde/plasma/screenlocker"_s, this, QDBusConnection::ExportAdaptors)) {
        setUnavailable();
        return;
    }
    m_dbusWorker = std::make_unique<OrgKdePlasmaScreenlockerWorkerInterface>(QString(), u"/org/kde/plasma/screenlocker/worker"_s, c);
    m_dbusWorker->setTimeout(std::numeric_limits<int>::max());
}

#include "moc_pamauthenticator.cpp"
