/*
    SPDX-FileCopyrightText: 2023 Janet Blackquill <uhhadd@gmail.com>
    SPDX-FileCopyrightText: 2026 Harald Sitter <sitter@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pamauthenticators.h"
#include "config-worker.h"
#include "kscreenlocker_greet_logging.h"
#include "pamauthenticatormodel.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <QMetaEnum>
#include <QPointer>
#include <QThread>
#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

struct PamAuthenticators::Private {
    std::unique_ptr<PamAuthenticator> interactive;
    std::vector<std::unique_ptr<PamAuthenticator>> noninteractive;
    PamAuthenticator::NoninteractiveAuthenticatorTypes computedTypes = PamAuthenticator::None;
    AuthenticatorsState state = Idle;
    bool configured = true;
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
        computedTypes = PamAuthenticator::None;
        for (const auto &authenticator : noninteractive) {
            if (authenticator->isAvailable()) {
                computedTypes |= authenticator->authenticatorType();
            }
        }
    }

    void cancelNoninteractive()
    {
        for (const auto &authenticator : noninteractive) {
            authenticator->cancel();
        }
    }
};

PamAuthenticators::PamAuthenticators(const QString &loginName, QObject *parent)
    : QObject(parent)
    , m_loginName(loginName)
    , d(new Private)
{
    connect(this, &PamAuthenticators::authenticatorChanged, this, &PamAuthenticators::onAuthenticatorChanged);
    // Initialize synchronously so the legacy QML context never sees null authenticators.
    onAuthenticatorChanged();
}

PamAuthenticators::PamAuthenticators(std::unique_ptr<PamAuthenticator> &&interactive,
                                     std::vector<std::unique_ptr<PamAuthenticator>> &&noninteractive,
                                     QObject *parent)
    : QObject(parent)
    , d(new Private)
{
    d->configured = false;
    d->interactive = std::move(interactive);
    d->noninteractive = std::move(noninteractive);
    connectAuthenticators();
}

PamAuthenticators::~PamAuthenticators() = default;

PamAuthenticators::Authenticator PamAuthenticators::loadAuthenticatorType()
{
    const auto enumInfo = QMetaEnum::fromType<Authenticator>();
    const auto group = KSharedConfig::openStateConfig(u"kscreenlockerrc"_s)->group(u"Greeter"_s);
    const auto key = group.readEntry("Authenticator", "Regular");
    bool valid = false;
    const int value = enumInfo.keyToValue(key.toUtf8().constData(), &valid);
    return valid ? static_cast<Authenticator>(value) : Authenticator::Regular;
}

void PamAuthenticators::saveAuthenticatorType(Authenticator authenticator)
{
    if (!d->configured) {
        return;
    }
    const auto enumInfo = QMetaEnum::fromType<Authenticator>();
    auto group = KSharedConfig::openStateConfig(u"kscreenlockerrc"_s)->group(u"Greeter"_s);
    group.writeEntry("Authenticator", enumInfo.valueToKey(std::to_underlying(authenticator)));
    group.sync();
}

void PamAuthenticators::onAuthenticatorChanged()
{
    const bool wasAuthenticating = d->state == Authenticating;
    clearPendingResponse("authenticator changed");
    if (d->interactive) {
        d->interactive->disconnect(this);
        d->interactive->cancel();
    }
    for (const auto &authenticator : d->noninteractive) {
        authenticator->disconnect(this);
        authenticator->cancel();
    }
    d->noninteractive.clear();
    if (!PAMAuthenticatorModel::instance()->isFunctional(m_authenticator)) {
        m_authenticator = Authenticator::Regular;
    }
    const auto service = [this] {
        switch (m_authenticator) {
        case Authenticator::Regular:
            return KSCREENLOCKER_PAM_PASSWORD_SERVICE;
        case Authenticator::Fingerprint:
            return KSCREENLOCKER_PAM_FINGERPRINT_SERVICE;
        case Authenticator::Smartcard:
            return KSCREENLOCKER_PAM_SMARTCARD_SERVICE;
        case Authenticator::Face:
            return KSCREENLOCKER_PAM_FACE_SERVICE;
        case Authenticator::Universal2Factor:
            return KSCREENLOCKER_PAM_UNIVERSAL2FACTOR_SERVICE;
        }
        return KSCREENLOCKER_PAM_PASSWORD_SERVICE;
    }();
    // Keep the active authenticator's legacy type as None; secondary methods report their own type.
    d->interactive = std::make_unique<PamAuthenticator>(service, m_loginName);
    if (m_authenticator != Authenticator::Fingerprint && KSCREENLOCKER_PAM_FINGERPRINT_SERVICE != "disabled"_L1) {
        d->noninteractive.push_back(std::make_unique<PamAuthenticator>(KSCREENLOCKER_PAM_FINGERPRINT_SERVICE, m_loginName, PamAuthenticator::Fingerprint));
    }
    // Sonic's existing lockscreen exposes smartcard authentication as a secondary method.
    // Keep it available without requiring the new authenticator-selector UI.
    if (m_authenticator == Authenticator::Regular && KSCREENLOCKER_PAM_SMARTCARD_SERVICE != "disabled"_L1) {
        d->noninteractive.push_back(std::make_unique<PamAuthenticator>(KSCREENLOCKER_PAM_SMARTCARD_SERVICE, m_loginName, PamAuthenticator::Smartcard));
    }
    connectAuthenticators();
    d->hadPrompt = false;
    Q_EMIT hadPromptChanged();
    setState(Idle);
    Q_EMIT promptChanged();
    Q_EMIT promptForSecretChanged();
    Q_EMIT infoMessageChanged();
    Q_EMIT errorMessageChanged();
    Q_EMIT busyChanged();
    Q_EMIT inPasswordDelayChanged();
    // Do not clear graceLocked or suspended here: switching methods must not bypass those guards.
    if (wasAuthenticating) {
        startAuthenticating();
    }
}

void PamAuthenticators::connectAuthenticators()
{
    auto interactive = d->interactive.get();
    connect(interactive, &PamAuthenticator::succeeded, this, [this] {
        clearPendingResponse("authentication succeeded");
        d->cancelNoninteractive();
        saveAuthenticatorType(m_authenticator);
        setState(Idle);
        Q_EMIT succeeded();
    });
    connect(interactive, &PamAuthenticator::failed, this, [this] {
        clearPendingResponse("authentication failed");
        setState(Idle);
        d->cancelNoninteractive();
        Q_EMIT failed(PamAuthenticator::None, d->interactive.get());
    });
    connect(interactive, &PamAuthenticator::loginFailedDelayStarted, this, [this](uint useconds) {
        Q_EMIT loginFailedDelayStarted(PamAuthenticator::None, d->interactive.get(), useconds);
    });
    connect(interactive, &PamAuthenticator::inPasswordDelayChanged, this, [this] {
        Q_EMIT inPasswordDelayChanged();
        flushPendingResponse();
    });
    connect(interactive, &PamAuthenticator::promptReadyChanged, this, [this] {
        if (d->interactive->isPromptReady()) {
            if (d->responseInFlight) {
                d->responseInFlight = false;
                Q_EMIT hasPendingResponseChanged();
            }
            flushPendingResponse();
        }
    });
    connect(interactive, &PamAuthenticator::authenticationCancelled, this, [this] {
        clearPendingResponse("authentication cancelled");
        setState(Idle);
    });
    connect(interactive, &PamAuthenticator::availableChanged, this, [this] {
        if (d->interactive->isUnavailable()) {
            clearPendingResponse("interactive authenticator unavailable");
            setState(Idle);
            if (d->configured) {
                PAMAuthenticatorModel::instance()->markDefunct(m_authenticator);
            }
        }
    });
    connect(interactive, &PamAuthenticator::busyChanged, this, &PamAuthenticators::busyChanged);
    auto hadPrompt = [this] {
        if (!d->hadPrompt) {
            d->hadPrompt = true;
            Q_EMIT hadPromptChanged();
        }
    };
    connect(interactive, &PamAuthenticator::prompt, this, [this, hadPrompt] {
        hadPrompt();
        Q_EMIT promptChanged();
    });
    connect(interactive, &PamAuthenticator::promptForSecret, this, [this, hadPrompt] {
        hadPrompt();
        Q_EMIT promptForSecretChanged();
    });
    connect(interactive, &PamAuthenticator::infoMessage, this, &PamAuthenticators::infoMessageChanged);
    connect(interactive, &PamAuthenticator::errorMessage, this, &PamAuthenticators::errorMessageChanged);

    for (const auto &authenticator : d->noninteractive) {
        auto secondary = authenticator.get();
        connect(secondary, &PamAuthenticator::succeeded, this, [this] {
            clearPendingResponse("non-interactive authentication succeeded");
            d->interactive->cancel();
            saveAuthenticatorType(m_authenticator);
            setState(Idle);
            Q_EMIT succeeded();
        });
        connect(secondary, &PamAuthenticator::availableChanged, this, [this] {
            d->recomputeNoninteractiveAuthenticationTypes();
            Q_EMIT authenticatorTypesChanged();
        });
        connect(secondary, &PamAuthenticator::failed, this, [this, secondary] {
            Q_EMIT failed(secondary->authenticatorType(), secondary);
        });
        connect(secondary, &PamAuthenticator::loginFailedDelayStarted, this, [this, secondary](uint useconds) {
            Q_EMIT loginFailedDelayStarted(secondary->authenticatorType(), secondary, useconds);
        });
        connect(secondary, &PamAuthenticator::infoMessage, this, [this, secondary, hadPrompt] {
            hadPrompt();
            Q_EMIT noninteractiveInfo(secondary->authenticatorType(), secondary);
        });
        connect(secondary, &PamAuthenticator::errorMessage, this, [this, secondary] {
            Q_EMIT noninteractiveError(secondary->authenticatorType(), secondary);
        });
    }
    d->recomputeNoninteractiveAuthenticationTypes();
    Q_EMIT authenticatorTypesChanged();
}

bool PamAuthenticators::isUnlocked() const
{
    return (d->interactive && d->interactive->isUnlocked()) || std::ranges::any_of(d->noninteractive, [](const auto &authenticator) {
               return authenticator->isUnlocked();
           });
}

PamAuthenticators::AuthenticatorsState PamAuthenticators::state() const
{
    return d->state;
}

void PamAuthenticators::startAuthenticating()
{
    if (d->state == Authenticating || d->graceLocked || d->suspended || isUnlocked() || !d->interactive || d->interactive->isUnavailable()) {
        return;
    }
    setState(Authenticating);
    d->interactive->tryUnlock();
    for (const auto &authenticator : d->noninteractive) {
        authenticator->tryUnlock();
    }
}

void PamAuthenticators::stopAuthenticating()
{
    d->cancelNoninteractive();
    if (d->interactive) {
        d->interactive->cancel();
    }
    clearPendingResponse("authentication stopped");
    setState(Idle);
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
    if (d->state != state) {
        d->state = state;
        Q_EMIT stateChanged();
    }
}

bool PamAuthenticators::isBusy() const
{
    return d->interactive && d->interactive->isBusy();
}

QString PamAuthenticators::prompt() const
{
    return d->interactive ? d->interactive->getPrompt() : QString();
}

QString PamAuthenticators::promptForSecret() const
{
    return d->interactive ? d->interactive->getPromptForSecret() : QString();
}

QString PamAuthenticators::infoMessage() const
{
    return d->interactive ? d->interactive->getInfoMessage() : QString();
}

QString PamAuthenticators::errorMessage() const
{
    return d->interactive ? d->interactive->getErrorMessage() : QString();
}

void PamAuthenticators::respond(const QByteArray &response)
{
    if (d->interactive && !d->suspended && !d->graceLocked) {
        d->interactive->respond(response);
    }
}

bool PamAuthenticators::submitResponse(const QString &response, QObject *owner)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (!owner || owner->thread() != thread() || d->suspended || d->graceLocked || isUnlocked() || !d->interactive || d->interactive->isUnavailable()
        || d->state != Authenticating || d->hasPendingResponse || d->responseInFlight) {
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
    if (!d->hasPendingResponse || d->responseInFlight || inPasswordDelay() || !d->interactive || !d->interactive->isPromptReady()
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
    stopAuthenticating();
}

PamAuthenticator::NoninteractiveAuthenticatorTypes PamAuthenticators::authenticatorTypes() const
{
    return d->computedTypes;
}

void PamAuthenticators::setGraceLocked(bool locked)
{
    if (d->graceLocked == locked) {
        return;
    }
    d->graceLocked = locked;
    if (locked) {
        stopAuthenticating();
    }
}

bool PamAuthenticators::inPasswordDelay() const
{
    return d->interactive && d->interactive->inPasswordDelay();
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
