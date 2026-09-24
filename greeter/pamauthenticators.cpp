/*
    SPDX-FileCopyrightText: 2023 Janet Blackquill <uhhadd@gmail.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include <QDebug>
#include <QPointer>
#include <QThread>
#include <algorithm>
#include <utility>

#include "kscreenlocker_greet_logging.h"
#include "pamauthenticator.h"
#include "pamauthenticators.h"

struct PamAuthenticators::Private {
    Private(std::unique_ptr<PamAuthenticator> &&interactiveAuthenticator, std::vector<std::unique_ptr<PamAuthenticator>> &&noninteractiveAuthenticators)
        : interactive(std::move(interactiveAuthenticator))
        , noninteractive(std::move(noninteractiveAuthenticators))
    {
    }

    std::unique_ptr<PamAuthenticator> interactive;
    std::vector<std::unique_ptr<PamAuthenticator>> noninteractive;
    PamAuthenticator::NoninteractiveAuthenticatorTypes computedTypes = PamAuthenticator::NoninteractiveAuthenticatorType::None;
    AuthenticatorsState state = AuthenticatorsState::Idle;
    bool graceLocked = false;
    bool hadPrompt = false;
    bool suspended = false;
    bool responseInFlight = false;
    QByteArray pendingResponse;
    bool hasPendingResponse = false;
    quint64 pendingRequestId = 0;
    QPointer<QObject> pendingOwner;
    QMetaObject::Connection pendingOwnerDestroyedConnection;

    void recomputeNoninteractiveAuthenticationTypes()
    {
        PamAuthenticator::NoninteractiveAuthenticatorTypes result = PamAuthenticator::NoninteractiveAuthenticatorType::None;
        for (auto &&noninteractive : noninteractive) {
            if (noninteractive->isAvailable()) {
                result |= noninteractive->authenticatorType();
            }
        }
        computedTypes = result;
    }
    void cancelNoninteractive()
    {
        for (auto &&noninteractive : noninteractive) {
            noninteractive->cancel();
        }
    }
};

PamAuthenticators::PamAuthenticators(std::unique_ptr<PamAuthenticator> &&interactive,
                                     std::vector<std::unique_ptr<PamAuthenticator>> &&noninteractive,
                                     QObject *parent)
    : QObject(parent)
    , d(new Private(std::move(interactive), std::move(noninteractive)))
{
    connect(d->interactive.get(), &PamAuthenticator::succeeded, this, [this] {
        qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: Success from interactive authenticator" << qUtf8Printable(d->interactive->service());
        clearPendingResponse("authentication succeeded");
        setState(AuthenticatorsState::Idle);
        Q_EMIT succeeded();
    });
    connect(d->interactive.get(), &PamAuthenticator::failed, this, [this] {
        qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: Failure from interactive authenticator" << qUtf8Printable(d->interactive->service());
        clearPendingResponse("authentication failed");
        setState(AuthenticatorsState::Idle);
        d->cancelNoninteractive();
        Q_EMIT failed(PamAuthenticator::NoninteractiveAuthenticatorType::None, d->interactive.get());
    });
    connect(d->interactive.get(), &PamAuthenticator::loginFailedDelayStarted, this, [this](const uint uSecDelay) noexcept -> void {
        qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: Delay started on login failure for interactive authenticator" << qUtf8Printable(d->interactive->service())
        << "duration:" << uSecDelay;
        Q_EMIT loginFailedDelayStarted(PamAuthenticator::NoninteractiveAuthenticatorType::None, d->interactive.get(), uSecDelay);
    });
    connect(d->interactive.get(), &PamAuthenticator::inPasswordDelayChanged, this, [this] {
        Q_EMIT inPasswordDelayChanged();
        flushPendingResponse();
    });
    connect(d->interactive.get(), &PamAuthenticator::promptReadyChanged, this, [this] {
        if (d->interactive->isPromptReady()) {
            if (d->responseInFlight) {
                d->responseInFlight = false;
                Q_EMIT hasPendingResponseChanged();
            }
            flushPendingResponse();
        }
    });
    connect(d->interactive.get(), &PamAuthenticator::authenticationCancelled, this, [this] {
        clearPendingResponse("authentication cancelled");
        setState(AuthenticatorsState::Idle);
    });
    connect(d->interactive.get(), &PamAuthenticator::availableChanged, this, [this] {
        if (d->interactive->isUnavailable()) {
            clearPendingResponse("interactive authenticator unavailable");
            setState(AuthenticatorsState::Idle);
        }
    });
    for (auto &&noninteractive : d->noninteractive) {
        connect(noninteractive.get(), &PamAuthenticator::succeeded, this, [this, &noninteractive] {
            qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: Success from non-interactive authenticator" << qUtf8Printable(noninteractive->service());
            clearPendingResponse("non-interactive authentication succeeded");
            d->interactive->cancel();
            setState(AuthenticatorsState::Idle);
            Q_EMIT succeeded();
        });
        connect(noninteractive.get(), &PamAuthenticator::availableChanged, this, [this, &noninteractive] {
            qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: Availability changed for non-interactive authenticator"
                                         << qUtf8Printable(noninteractive->service()) << noninteractive->isAvailable();
            d->recomputeNoninteractiveAuthenticationTypes();
            Q_EMIT authenticatorTypesChanged();
        });
        connect(noninteractive.get(), &PamAuthenticator::failed, this, [this, &noninteractive] {
            qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: Non-interactive authenticator" << qUtf8Printable(noninteractive->service()) << "failed";
            Q_EMIT failed(noninteractive->authenticatorType(), noninteractive.get());
        });
        connect(noninteractive.get(), &PamAuthenticator::loginFailedDelayStarted, this, [this, &noninteractive](const uint uSecDelay) noexcept -> void {
            qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: Delay started on login failure for non-interactive authenticator" << qUtf8Printable(noninteractive->service())
            << "duration:" << uSecDelay;
            Q_EMIT loginFailedDelayStarted(noninteractive->authenticatorType(), noninteractive.get(), uSecDelay);
        });
        connect(noninteractive.get(), &PamAuthenticator::infoMessage, this, [this, &noninteractive]() {
            if (!d->hadPrompt) {
                d->hadPrompt = true;
                Q_EMIT hadPromptChanged();
            }
            qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: Info message from non-interactive authenticator" << qUtf8Printable(noninteractive->service());
            Q_EMIT noninteractiveInfo(noninteractive->authenticatorType(), noninteractive.get());
        });
        connect(noninteractive.get(), &PamAuthenticator::errorMessage, this, [this, &noninteractive]() {
            qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: Error message from non-interactive authenticator " << qUtf8Printable(noninteractive->service());
            Q_EMIT noninteractiveError(noninteractive->authenticatorType(), noninteractive.get());
        });
    }

    // connect the delegated signals
    connect(d->interactive.get(), &PamAuthenticator::busyChanged, this, [this] {
        qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: Interactive authenticator" << qUtf8Printable(d->interactive->service()) << "changed business";
        Q_EMIT busyChanged();
    });
    connect(d->interactive.get(), &PamAuthenticator::prompt, this, [this] {
        if (!d->hadPrompt) {
            d->hadPrompt = true;
            Q_EMIT hadPromptChanged();
        }
        qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: Normal prompt from interactive authenticator" << qUtf8Printable(d->interactive->service());
        Q_EMIT promptChanged();
    });
    connect(d->interactive.get(), &PamAuthenticator::promptForSecret, this, [this] {
        if (!d->hadPrompt) {
            d->hadPrompt = true;
            Q_EMIT hadPromptChanged();
        }
        qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: Secret prompt from interactive authenticator" << qUtf8Printable(d->interactive->service());
        Q_EMIT promptForSecretChanged();
    });
    connect(d->interactive.get(), &PamAuthenticator::infoMessage, this, [this] {
        qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: Info message from interactive authenticator" << qUtf8Printable(d->interactive->service());
        Q_EMIT infoMessageChanged();
    });
    connect(d->interactive.get(), &PamAuthenticator::errorMessage, this, [this] {
        qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: Error message from interactive authenticator" << qUtf8Printable(d->interactive->service());
        Q_EMIT errorMessageChanged();
    });
}

PamAuthenticators::~PamAuthenticators()
{
}

bool PamAuthenticators::isUnlocked() const
{
    return d->interactive->isUnlocked() || std::any_of(d->noninteractive.cbegin(), d->noninteractive.cend(), [](auto &&t) {
               return t->isUnlocked();
           });
}

PamAuthenticators::AuthenticatorsState PamAuthenticators::state() const
{
    return d->state;
}

void PamAuthenticators::startAuthenticating()
{
    if (d->state == AuthenticatorsState::Authenticating || d->graceLocked || d->suspended || isUnlocked() || d->interactive->isUnavailable()) {
        return;
    }

    qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: starting authenticators";
    setState(AuthenticatorsState::Authenticating);
    d->interactive->tryUnlock();
    for (auto &&noninteractive : d->noninteractive) {
        noninteractive->tryUnlock();
    }
}

void PamAuthenticators::stopAuthenticating()
{
    qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: stopping authenticators";
    for (auto &&noninteractive : d->noninteractive) {
        noninteractive->cancel();
    }
    d->interactive->cancel();
    clearPendingResponse("authentication stopped");
    setState(AuthenticatorsState::Idle);
}

void PamAuthenticators::setSuspended(bool suspended)
{
    if (d->suspended == suspended) {
        return;
    }
    d->suspended = suspended;
    Q_EMIT suspendedChanged();
    if (suspended) {
        stopAuthenticating();
    } else {
        startAuthenticating();
    }
}

void PamAuthenticators::setState(AuthenticatorsState state)
{
    if (d->state == state) {
        return;
    }
    qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: state changing from" << d->state << "to" << state;
    d->state = state;
    Q_EMIT stateChanged();
}

// these properties are delegated to interactive authenticator

bool PamAuthenticators::isBusy() const
{
    return d->interactive->isBusy();
}

QString PamAuthenticators::prompt() const
{
    return d->interactive->getPrompt();
}

QString PamAuthenticators::promptForSecret() const
{
    return d->interactive->getPromptForSecret();
}

QString PamAuthenticators::infoMessage() const
{
    return d->interactive->getInfoMessage();
}

QString PamAuthenticators::errorMessage() const
{
    return d->interactive->getErrorMessage();
}

void PamAuthenticators::respond(const QByteArray &response)
{
    qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: responding to interactive authenticator";
    d->interactive->respond(response);
}

bool PamAuthenticators::submitResponse(const QString &response, QObject *owner)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (!owner || owner->thread() != thread() || d->suspended || d->graceLocked || isUnlocked() || d->interactive->isUnavailable()
        || d->state != AuthenticatorsState::Authenticating || d->hasPendingResponse || d->responseInFlight) {
        return false;
    }

    if (d->interactive->isPromptReady() && !inPasswordDelay()) {
        d->responseInFlight = true;
        Q_EMIT hasPendingResponseChanged();
        d->interactive->respond(response.toUtf8());
        return true;
    }

    d->pendingResponse = response.toUtf8();
    d->hasPendingResponse = true;
    d->pendingRequestId = d->interactive->currentRequestId();
    d->pendingOwner = owner;
    d->pendingOwnerDestroyedConnection = connect(owner, &QObject::destroyed, this, [this] {
        clearPendingResponse("submitting view destroyed");
    });
    qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: queued response for request" << d->pendingRequestId;
    Q_EMIT hasPendingResponseChanged();
    flushPendingResponse();
    return true;
}

void PamAuthenticators::discardPendingResponse()
{
    clearPendingResponse("explicitly discarded");
}

void PamAuthenticators::clearPendingResponse(const char *reason)
{
    const bool hadSubmission = d->hasPendingResponse || d->responseInFlight;
    if (d->pendingOwnerDestroyedConnection) {
        disconnect(d->pendingOwnerDestroyedConnection);
        d->pendingOwnerDestroyedConnection = {};
    }
    d->pendingResponse.clear();
    d->hasPendingResponse = false;
    d->responseInFlight = false;
    d->pendingRequestId = 0;
    d->pendingOwner.clear();
    if (hadSubmission) {
        qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: cleared pending response:" << reason;
        Q_EMIT hasPendingResponseChanged();
    }
}

void PamAuthenticators::flushPendingResponse()
{
    if (!d->hasPendingResponse || d->responseInFlight || inPasswordDelay() || !d->interactive->isPromptReady()
        || d->pendingRequestId != d->interactive->currentRequestId() || d->suspended || d->graceLocked || isUnlocked()) {
        return;
    }
    const QByteArray response = std::exchange(d->pendingResponse, {});
    if (d->pendingOwnerDestroyedConnection) {
        disconnect(d->pendingOwnerDestroyedConnection);
        d->pendingOwnerDestroyedConnection = {};
    }
    d->pendingOwner.clear();
    d->pendingRequestId = 0;
    d->hasPendingResponse = false;
    d->responseInFlight = true;
    Q_EMIT hasPendingResponseChanged();
    d->interactive->respond(response);
}

void PamAuthenticators::cancel()
{
    qCDebug(KSCREENLOCKER_GREET) << "PamAuthenticators: cancelling interactive authenticator";
    d->interactive->cancel();
    clearPendingResponse("interactive authentication cancelled");
    setState(AuthenticatorsState::Idle);
}

PamAuthenticator::NoninteractiveAuthenticatorTypes PamAuthenticators::authenticatorTypes() const
{
    return d->computedTypes;
}

void PamAuthenticators::setGraceLocked(bool b)
{
    if (d->graceLocked == b) {
        return;
    }
    d->graceLocked = b;
    if (b) {
        stopAuthenticating();
    }
}

bool PamAuthenticators::inPasswordDelay() const
{
    return d->interactive->inPasswordDelay();
}

bool PamAuthenticators::hasPendingResponse() const
{
    return d->hasPendingResponse || d->responseInFlight;
}

bool PamAuthenticators::isSuspended() const
{
    return d->suspended;
}

bool PamAuthenticators::hadPrompt() const
{
    return d->hadPrompt;
}
