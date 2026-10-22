/*
 * SPDX-FileCopyrightText: 2026 SonicDE Community
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "virtualkeyboard.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>

namespace ScreenLocker {
VirtualKeyboard::VirtualKeyboard(QObject *parent)
    : QObject(parent)
{
    connect(qGuiApp, &QGuiApplication::focusWindowChanged, this, [this](QWindow *window) {
        if (auto *view = qobject_cast<QQuickWindow *>(window); m_views.contains(view)) {
            activateView(view);
        }
    });
}

VirtualKeyboard::~VirtualKeyboard()
{
    delete m_panel;
}

void VirtualKeyboard::addView(QQuickWindow *view, QQmlEngine *engine)
{
    if (!view || !engine || m_views.contains(view)) {
        return;
    }
    m_views.insert(view, engine);
    connect(view, &QObject::destroyed, this, [this, view] {
        m_views.remove(view);
    });
    if (!m_panel || QGuiApplication::focusWindow() == view) {
        activateView(view);
    }
}

void VirtualKeyboard::removeView(QQuickWindow *view)
{
    const auto engine = m_views.take(view);
    if (m_panel && (m_panel->window() == view || (m_panel->parent() == engine && !m_views.values().contains(engine)))) {
        // Destroy QML while its engine's style singletons and visual parent are
        // still alive, before a removed screen tears down its view/engine.
        delete m_panel;
    }
    if (!m_panel && !m_views.isEmpty()) {
        auto *focused = qobject_cast<QQuickWindow *>(QGuiApplication::focusWindow());
        activateView(m_views.contains(focused) ? focused : m_views.constBegin().key());
    }
}

void VirtualKeyboard::activateView(QQuickWindow *view)
{
    auto *engine = m_views.value(view).data();
    if (!engine) {
        return;
    }
    if (!m_panel) {
        QQmlComponent component(engine, QUrl(QStringLiteral("qrc:/kscreenlocker/virtualkeyboard.qml")));
        auto *item = qobject_cast<QQuickItem *>(component.beginCreate(engine->rootContext()));
        if (!item) {
            component.completeCreate();
            qWarning() << "Cannot create screenlocker input panel:" << component.errors();
            return;
        }
        item->setParent(engine);
        item->setParentItem(view->contentItem());
        component.completeCreate();
        if (component.isError()) {
            qWarning() << "Cannot initialize screenlocker input panel:" << component.errors();
            delete item;
            return;
        }
        QQmlEngine::setObjectOwnership(item, QQmlEngine::CppOwnership);
        m_panel = item;
    } else {
        m_panel->setParentItem(view->contentItem());
    }
}

QQuickItem *VirtualKeyboard::panel() const
{
    return m_panel;
}
}
