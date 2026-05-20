/*
    SPDX-FileCopyrightText: 2026 Sonic DE Team
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QObject>

namespace ScreenLocker
{
class KSldApp;
}

/**
 * D-Bus adaptor for the greeter interface.
 * Implements the methods defined in org.kde.screensaver.Greeter.xml
 */
class GreeterAdaptor : public QObject
{
    Q_OBJECT

public:
    explicit GreeterAdaptor(ScreenLocker::KSldApp *parent);
    ~GreeterAdaptor() override = default;

public Q_SLOTS:
    void RegisterWindow(uint winId, const QString &screenName);
    void UnregisterWindow(uint winId);
    void AuthenticationSuccess();
    void GetFocus(const QString &screenName);

Q_SIGNALS:
    void ScreenAdded(const QString &screenName, int x, int y, int width, int height);
    void ScreenRemoved(const QString &screenName);
    void LockRequested();
    void UnlockRequested();

private:
    ScreenLocker::KSldApp *m_ksldApp;
};
