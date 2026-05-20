/*
    SPDX-FileCopyrightText: 2026 Sonic DE Team
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "greeteradaptor.h"
#include "kscreenlocker_logging.h"
#include "ksldapp.h"

#include <QDBusConnection>

using namespace ScreenLocker;

GreeterAdaptor::GreeterAdaptor(KSldApp *parent)
    : QObject(parent)
    , m_ksldApp(parent)
{
    // Register this object on the session bus
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/Greeter"), this, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals);
    QDBusConnection::sessionBus().registerService(QStringLiteral("org.kde.screensaver"));
}

void GreeterAdaptor::RegisterWindow(uint winId, const QString &screenName)
{
    qCDebug(KSCREENLOCKER) << "Greeter registered window:" << winId << "for screen:" << screenName;
    if (m_ksldApp) {
        m_ksldApp->registerGreeterWindow(winId, screenName);
    }
}

void GreeterAdaptor::UnregisterWindow(uint winId)
{
    qCDebug(KSCREENLOCKER) << "Greeter unregistered window:" << winId;
    if (m_ksldApp) {
        m_ksldApp->unregisterGreeterWindow(winId);
    }
}

void GreeterAdaptor::AuthenticationSuccess()
{
    qCDebug(KSCREENLOCKER) << "Greeter reported authentication success";
    if (m_ksldApp) {
        m_ksldApp->greeterAuthenticationSuccess();
    }
}

void GreeterAdaptor::GetFocus(const QString &screenName)
{
    qCDebug(KSCREENLOCKER) << "Greeter requested focus for screen:" << screenName;
    if (m_ksldApp) {
        m_ksldApp->greeterGetFocus(screenName);
    }
}
