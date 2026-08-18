/*
    SPDX-FileCopyrightText: 2020 David Edmundson <davidedmundson@kde.org>
    SPDX-FileCopyrightText: 2026 Harald Sitter <sitter@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pamauthenticator.h"
#include "kscreenlocker_greet_logging.h"
#include "pamworker.h"

PamAuthenticator::PamAuthenticator(const QString &service, const QString &user, NoninteractiveAuthenticatorTypes types, QObject *parent)
    : QObject(parent)
    , m_service(service)
    , m_authenticatorType(types)
    , m_thread(std::make_unique<PamWorker>(service, user))
    , d(m_thread.worker())
{
    connect(d, &PamWorker::busyChanged, this, &PamAuthenticator::setBusy);
    connect(d, &PamWorker::prompt, this, [this](quint64 requestId, const QString &msg) {
        if (requestId != m_requestId) {
            return;
        }
        m_prompt = msg;
        m_hadPromptForRequest = true;
        m_promptReady = true;
        Q_EMIT promptReadyChanged();
        Q_EMIT prompt(msg);
    });
    connect(d, &PamWorker::promptForSecret, this, [this](quint64 requestId, const QString &msg) {
        if (requestId != m_requestId) {
            return;
        }
        m_promptForSecret = msg;
        m_hadPromptForRequest = true;
        m_promptReady = true;
        Q_EMIT promptReadyChanged();
        Q_EMIT promptForSecret(msg);
    });
    connect(d, &PamWorker::infoMessage, this, [this](const QString &msg) {
        m_infoMessage = msg;
        Q_EMIT infoMessage(msg);
    });
    connect(d, &PamWorker::errorMessage, this, [this](const QString &msg) {
        m_errorMessage = msg;
        Q_EMIT errorMessage(msg);
    });
    connect(d, &PamWorker::unavailabilityChanged, this, [this](bool unavailable) {
        m_unavailable = unavailable;
        if (unavailable) {
            m_authenticationRequested = false;
            clearPromptState();
        }
        Q_EMIT availableChanged();
    });
    connect(d, &PamWorker::succeeded, this, [this](quint64 requestId) {
        if (requestId != m_requestId) {
            return;
        }
        clearPromptState();
        m_unlocked = true;
        m_authenticationRequested = false;
        Q_EMIT succeeded();
    });
    connect(d, &PamWorker::failed, this, [this](quint64 requestId) {
        if (requestId != m_requestId) {
            return;
        }
        clearPromptState();
        m_authenticationRequested = false;

        // Stop retry loops from broken services that never issue a prompt, such as pam-u2f
        // without a token. Ordinary wrong-password attempts must not disable the password backend.
        if (m_hadPromptForRequest) {
            m_failedCount = 0;
        } else {
            const auto now = std::chrono::steady_clock::now();
            if (now - m_lastFailed < std::chrono::seconds(2)) {
                ++m_failedCount;
            } else {
                m_failedCount = 1;
            }
            m_lastFailed = now;
            if (m_failedCount >= 4) {
                m_unavailable = true;
                Q_EMIT availableChanged();
            }
        }
        Q_EMIT failed();
    });
    connect(d, &PamWorker::authenticationCancelled, this, [this](quint64 requestId) {
        if (requestId != m_requestId) {
            return;
        }
        clearPromptState();
        m_authenticationRequested = false;
        Q_EMIT authenticationCancelled();
    });
    connect(d, &PamWorker::loginFailedDelayStarted, this, &PamAuthenticator::loginFailedDelayStarted);
    connect(d, &PamWorker::inPasswordDelayChanged, this, &PamAuthenticator::setInPasswordDelay);
    m_thread.start();
    QMetaObject::invokeMethod(d, &PamWorker::start, Qt::QueuedConnection);
}

PamAuthenticator::~PamAuthenticator()
{
    m_thread.quit();
    m_thread.wait();
}

bool PamAuthenticator::isBusy() const
{
    return m_busy;
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

bool PamAuthenticator::isUnlocked() const
{
    return m_unlocked;
}

void PamAuthenticator::tryUnlock()
{
    if (m_unlocked || m_unavailable || m_authenticationRequested) {
        return;
    }
    m_authenticationRequested = true;
    m_hadPromptForRequest = false;
    clearPromptState();
    const quint64 requestId = ++m_requestId;
    QMetaObject::invokeMethod(
        d,
        [worker = d, requestId] {
            worker->authenticate(requestId);
        },
        Qt::QueuedConnection);
}

void PamAuthenticator::respond(const QByteArray &response)
{
    if (!m_promptReady) {
        qCDebug(KSCREENLOCKER_GREET) << "Ignoring response without an active PAM prompt";
        return;
    }
    m_promptReady = false;
    Q_EMIT promptReadyChanged();
    const quint64 requestId = m_requestId;
    QMetaObject::invokeMethod(
        d,
        [worker = d, requestId, response] {
            worker->respond(requestId, response);
        },
        Qt::QueuedConnection);
}

void PamAuthenticator::cancel()
{
    const quint64 requestId = m_requestId;
    clearPromptState();
    m_authenticationRequested = false;
    // Invalidate any result already queued by the old worker before cancellation reaches it.
    ++m_requestId;
    QMetaObject::invokeMethod(
        d,
        [worker = d, requestId] {
            worker->cancel(requestId);
        },
        Qt::QueuedConnection);
}

void PamAuthenticator::setInPasswordDelay(bool inPasswordDelay)
{
    if (m_inPasswordDelay == inPasswordDelay) {
        return;
    }
    m_inPasswordDelay = inPasswordDelay;
    Q_EMIT inPasswordDelayChanged();
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

PamAuthenticator::WorkerThread::WorkerThread(std::unique_ptr<PamWorker> &&worker, QObject *parent)
    : QThread(parent)
    , m_worker(std::move(worker))
{
    m_worker->moveToThread(this);
}

PamWorker *PamAuthenticator::WorkerThread::worker() const
{
    return m_worker.get();
}

void PamAuthenticator::WorkerThread::run()
{
    QThread::run();
    m_worker.reset();
}

#include "moc_pamauthenticator.cpp"
