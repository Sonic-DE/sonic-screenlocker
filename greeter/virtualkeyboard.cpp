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

#include <qpa/qwindowsysteminterface.h>

#include <xcb/xcb.h>

namespace ScreenLocker {
VirtualKeyboard::VirtualKeyboard(QObject *parent)
    : QObject(parent)
{
    qGuiApp->installNativeEventFilter(this);
    connect(qGuiApp, &QGuiApplication::focusWindowChanged, this, [this](QWindow *window) {
        if (auto *view = qobject_cast<QQuickWindow *>(window); m_views.contains(view)) {
            activateView(view);
        }
    });
}

VirtualKeyboard::~VirtualKeyboard()
{
    if (qGuiApp) {
        qGuiApp->removeNativeEventFilter(this);
    }
    delete m_panel;
}

bool VirtualKeyboard::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *)
{
    if (eventType != QByteArrayLiteral("xcb_generic_event_t")) {
        return false;
    }
    const auto *event = static_cast<xcb_generic_event_t *>(message);
    const auto type = event->response_type & ~0x80;
    // Normal device events still belong to Qt's XInput path. Only bridge the
    // core wheel buttons synthesized by the daemon's native grab forwarding.
    if (!(event->response_type & 0x80) || (type != XCB_BUTTON_PRESS && type != XCB_BUTTON_RELEASE)) {
        return false;
    }
    const auto *button = reinterpret_cast<const xcb_button_press_event_t *>(event);
    if (button->detail < 4 || button->detail > 7) {
        return false;
    }
    for (auto *view : m_views.keys()) {
        if (view->winId() != button->event) {
            continue;
        }
        if (type == XCB_BUTTON_PRESS) {
            QPoint delta;
            switch (button->detail) {
            case 4:
                delta.setY(120);
                break;
            case 5:
                delta.setY(-120);
                break;
            case 6:
                delta.setX(120);
                break;
            case 7:
                delta.setX(-120);
                break;
            }
            // Enter through Qt's native-input queue, including its pointer
            // state and spontaneous-event delivery, not ordinary sendEvent().
            QWindowSystemInterface::handleWheelEvent(view,
                                                     button->time,
                                                     QPointF(button->event_x, button->event_y),
                                                     QPointF(button->root_x, button->root_y),
                                                     QPoint(),
                                                     delta,
                                                     QGuiApplication::queryKeyboardModifiers(),
                                                     Qt::NoScrollPhase,
                                                     Qt::MouseEventNotSynthesized,
                                                     false);
        }
        // Core wheel releases must not produce a second scroll or leak into
        // Qt's ordinary button handling. Other/unregistered windows are ignored.
        return true;
    }
    return false;
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
