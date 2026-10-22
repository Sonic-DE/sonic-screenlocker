/*
 * SPDX-FileCopyrightText: 2026 SonicDE Community
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>

class QQmlEngine;
class QQuickItem;
class QQuickWindow;

namespace ScreenLocker {
/** Hosts Qt's existing input panel only in registered greeter scenes. */
class VirtualKeyboard : public QObject {
public:
    explicit VirtualKeyboard(QObject *parent = nullptr);
    ~VirtualKeyboard() override;
    void addView(QQuickWindow *view, QQmlEngine *engine);
    void removeView(QQuickWindow *view);
    QQuickItem *panel() const;

private:
    void activateView(QQuickWindow *view);
    QHash<QQuickWindow *, QPointer<QQmlEngine>> m_views;
    QPointer<QQuickItem> m_panel;
};
}
